// Package hostconfig reads and edits capture-eye's JSON config file — the one
// the NixOS module points the service at with --config, and the only place a
// camera or serial-port change persists across a restart.
//
// The file is edited as a generic JSON object rather than through a Go struct
// mirroring capture-eye's AppConfig. Two reasons: a Go-side schema would drift
// from capture-eye/src/config_file.cpp the first time a key was added, and
// keys this build of horusctl has never heard of must survive a round-trip
// untouched. Correctness comes from capture-eye's own parser instead — see
// Validate.
package hostconfig

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

// DefaultPath matches services.horus.configFile's default in nix/module.nix.
const DefaultPath = "/etc/horus/capture-eye.json"

// EnvPath lets the module's horusctl wrapper hand the CLI the same path the
// unit reads, so an overridden services.horus.configFile needs no --config.
const EnvPath = "HORUS_CONFIG"

// ResolvePath applies: --config flag > $HORUS_CONFIG > DefaultPath.
func ResolvePath(flagValue string) string {
	if flagValue != "" {
		return flagValue
	}
	if env := os.Getenv(EnvPath); env != "" {
		return env
	}
	return DefaultPath
}

// Doc is a parsed config file. A missing file loads as an empty object: every
// key is optional (capture-eye falls back to its built-in defaults), so "not
// there yet" and "there but empty" are genuinely the same starting point.
type Doc struct {
	Path string
	root map[string]any
	// mode of the file as found, so Save doesn't silently widen permissions on
	// a config someone deliberately locked down.
	mode os.FileMode
	// existed distinguishes "we are creating this" for the caller's messages.
	existed bool
}

// Load reads path. It does not validate: an existing file capture-eye would
// reject still loads, so `config set` can be used to *fix* one.
func Load(path string) (*Doc, error) {
	doc := &Doc{Path: path, root: map[string]any{}, mode: 0o644}

	raw, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return doc, nil
	}
	if err != nil {
		return nil, err
	}
	doc.existed = true
	if info, err := os.Stat(path); err == nil {
		doc.mode = info.Mode().Perm()
	}

	if len(bytes.TrimSpace(raw)) == 0 {
		return doc, nil
	}
	// UseNumber keeps 1280 as "1280" rather than float64(1280), which would
	// re-render as 1.28e+03 and make capture-eye reject a key nobody touched.
	dec := json.NewDecoder(bytes.NewReader(raw))
	dec.UseNumber()
	if err := dec.Decode(&doc.root); err != nil {
		return nil, fmt.Errorf("%s: not valid JSON: %w", path, err)
	}
	if doc.root == nil {
		doc.root = map[string]any{}
	}
	return doc, nil
}

// Existed reports whether the file was already on disk when it was loaded.
func (d *Doc) Existed() bool { return d.existed }

// Get returns the value at a dot-path such as "capture.device".
func (d *Doc) Get(path string) (any, error) {
	keys, err := splitPath(path)
	if err != nil {
		return nil, err
	}
	var cur any = d.root
	for i, key := range keys {
		obj, ok := cur.(map[string]any)
		if !ok {
			return nil, fmt.Errorf("%s is not an object", strings.Join(keys[:i], "."))
		}
		cur, ok = obj[key]
		if !ok {
			return nil, fmt.Errorf("%s is not set (capture-eye's built-in default applies)", path)
		}
	}
	return cur, nil
}

// Set writes value at a dot-path, creating intermediate objects as needed.
func (d *Doc) Set(path string, value any) error {
	keys, err := splitPath(path)
	if err != nil {
		return err
	}
	obj := d.root
	for i, key := range keys[:len(keys)-1] {
		switch child := obj[key].(type) {
		case map[string]any:
			obj = child
		case nil:
			created := map[string]any{}
			obj[key] = created
			obj = created
		default:
			return fmt.Errorf("%s already holds a value, not an object", strings.Join(keys[:i+1], "."))
		}
	}
	obj[keys[len(keys)-1]] = value
	return nil
}

// Unset deletes a dot-path, so capture-eye's built-in default applies again.
// Removing a key that isn't there is not an error — the end state is what was
// asked for either way.
func (d *Doc) Unset(path string) error {
	keys, err := splitPath(path)
	if err != nil {
		return err
	}
	obj := d.root
	for _, key := range keys[:len(keys)-1] {
		child, ok := obj[key].(map[string]any)
		if !ok {
			return nil
		}
		obj = child
	}
	delete(obj, keys[len(keys)-1])
	return nil
}

// Render returns the file's bytes: 2-space indent, keys sorted (encoding/json
// sorts map keys), trailing newline. Formatting is not preserved across an
// edit — JSON has no comments to lose, and a stable canonical shape makes
// `git diff` on a copied-out config readable.
func (d *Doc) Render() ([]byte, error) {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetIndent("", "  ")
	enc.SetEscapeHTML(false)
	if err := enc.Encode(d.root); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

// Save validates the rendered document with capture-eye itself and, only if
// that passes, replaces the file atomically. A rejected edit leaves the live
// config exactly as it was, which is what makes this safe to run against a
// running service.
func (d *Doc) Save(bin string) error {
	return d.install(func(path string) error { return Validate(bin, path) })
}

// SaveUnchecked installs the file without running capture-eye. Only for the
// case where no capture-eye binary exists to validate with — a workstation
// editing a config destined for another host — and the caller has said so.
func (d *Doc) SaveUnchecked() error {
	return d.install(nil)
}

func (d *Doc) install(validate func(string) error) error {
	if err := guardStorePath(d.Path); err != nil {
		return err
	}
	data, err := d.Render()
	if err != nil {
		return err
	}

	dir := filepath.Dir(d.Path)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	// Same directory as the target, so the rename below is atomic (a rename
	// across filesystems is not) and the temp file inherits the same mount.
	tmp, err := os.CreateTemp(dir, ".horusctl-*.json")
	if err != nil {
		return err
	}
	defer os.Remove(tmp.Name())

	if _, err := tmp.Write(data); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	if err := os.Chmod(tmp.Name(), d.mode); err != nil {
		return err
	}
	if validate != nil {
		if err := validate(tmp.Name()); err != nil {
			return err
		}
	}
	return os.Rename(tmp.Name(), d.Path)
}

// ErrNoBinary is returned by Validate when capture-eye isn't on PATH.
var ErrNoBinary = errors.New("capture-eye binary not found")

// Validate runs `capture-eye --check-config --config path`. That command opens
// no camera, no serial port and no model, so it is safe while the service is
// running — and it is capture-eye's real parser, so this cannot drift from
// what the service will accept two seconds later.
func Validate(bin, path string) error {
	resolved, err := exec.LookPath(bin)
	if err != nil {
		return fmt.Errorf("%w (%s)", ErrNoBinary, bin)
	}
	cmd := exec.Command(resolved, "--check-config", "--config", path)
	out, err := cmd.CombinedOutput()
	if err == nil {
		return nil
	}
	msg := strings.TrimSpace(string(out))
	if msg == "" {
		msg = err.Error()
	}
	return errors.New(msg)
}

// guardStorePath refuses to write into /nix/store. Pointing
// services.horus.configFile at a writeJSON derivation makes the live config
// read-only and rebuilt on every nixos-rebuild — the exact thing this CLI
// exists to avoid — so say so plainly instead of failing on EROFS.
func guardStorePath(path string) error {
	abs, err := filepath.Abs(path)
	if err != nil {
		abs = path
	}
	if !strings.HasPrefix(abs, "/nix/store/") {
		return nil
	}
	return fmt.Errorf("%s is in the Nix store and cannot be edited\n"+
		"services.horus.configFile must be a mutable path (the default, %s);\n"+
		"use services.horus.seedConfigFile to ship an initial config from Nix instead",
		abs, DefaultPath)
}

// ParseValue interprets a command-line value as JSON (30, true, "MJPG",
// [1,2]) and falls back to a bare string when it isn't valid JSON, so
// `set capture.device /dev/video0` needs no quoting. A wrong *type* is not
// guessed at here — capture-eye's strict parser catches it at Save time,
// which is the whole reason validation is delegated.
func ParseValue(text string) any {
	// json.Valid first, so trailing junk is rejected outright: a Decoder would
	// happily read 1280 out of "1280x720" and stop, turning a resolution into a
	// number.
	if !json.Valid([]byte(text)) {
		return text
	}
	dec := json.NewDecoder(strings.NewReader(text))
	dec.UseNumber()
	var value any
	if err := dec.Decode(&value); err != nil {
		return text
	}
	return value
}

// Format renders a value the way `config get` should print it: bare strings
// unquoted, everything else as JSON.
func Format(value any) string {
	if s, ok := value.(string); ok {
		return s
	}
	out, err := json.MarshalIndent(value, "", "  ")
	if err != nil {
		return fmt.Sprint(value)
	}
	return string(out)
}

func splitPath(path string) ([]string, error) {
	if path == "" {
		return nil, errors.New("empty key path")
	}
	keys := strings.Split(path, ".")
	for _, key := range keys {
		if key == "" {
			return nil, fmt.Errorf("malformed key path: %q", path)
		}
	}
	return keys, nil
}
