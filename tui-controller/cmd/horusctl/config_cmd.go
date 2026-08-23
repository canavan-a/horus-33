package main

import (
	"errors"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"strings"

	"github.com/canavan-a/horus-33/tui-controller/internal/hostconfig"
	"github.com/canavan-a/horus-33/tui-controller/internal/link"
	"github.com/canavan-a/horus-33/tui-controller/internal/service"
)

const configUsage = `horusctl config — edit capture-eye's config file

Usage: horusctl config <command> [options]

  show                      print the current config
  get KEY                   print one value, e.g. capture.device
  set KEY VALUE [KEY VALUE] set one or more keys
  unset KEY [KEY...]        drop keys, restoring capture-eye's defaults
  set-video [flags]         set the capture.* keys (see --help)
  set-serial [flags]        set the serial.* keys (see --help)
  edit                      open the file in $EDITOR, validate, then install
  devices                   list attached cameras and serial ports
  path                      print which file these commands act on

Common options:
  --config PATH        config file (default $HORUS_CONFIG, else ` + hostconfig.DefaultPath + `)
  --capture-eye PATH   capture-eye binary used to validate (default: from $PATH)
  --restart            restart ` + service.Unit + ` after a successful write

Every write is validated by capture-eye itself before it replaces the file, so
a rejected edit leaves the running config untouched. Camera and serial changes
only take effect on restart — capture-eye negotiates both once, at startup.
`

// configFlags are the options every mutating config subcommand shares.
type configFlags struct {
	path    string
	bin     string
	restart bool
}

func (c *configFlags) bind(fs *flag.FlagSet, withRestart bool) {
	fs.StringVar(&c.path, "config", "", "config file path")
	fs.StringVar(&c.bin, "capture-eye", "capture-eye", "capture-eye binary used to validate")
	if withRestart {
		fs.BoolVar(&c.restart, "restart", false, "restart "+service.Unit+" after writing")
	}
}

func runConfig(args []string) error {
	if len(args) == 0 {
		fmt.Print(configUsage)
		return nil
	}
	sub, rest := args[0], args[1:]
	switch sub {
	case "show":
		return configShow(rest)
	case "get":
		return configGet(rest)
	case "set":
		return configSet(rest)
	case "unset":
		return configUnset(rest)
	case "set-video":
		return configSetVideo(rest)
	case "set-serial":
		return configSetSerial(rest)
	case "edit":
		return configEdit(rest)
	case "devices":
		return configDevices(rest)
	case "path":
		return configPath(rest)
	case "-h", "--help", "help":
		fmt.Print(configUsage)
		return nil
	default:
		return fmt.Errorf("unknown config command %q (try: horusctl config --help)", sub)
	}
}

func configPath(args []string) error {
	var opts configFlags
	fs := flag.NewFlagSet("config path", flag.ContinueOnError)
	opts.bind(fs, false)
	if err := fs.Parse(args); err != nil {
		return err
	}
	fmt.Println(hostconfig.ResolvePath(opts.path))
	return nil
}

func configShow(args []string) error {
	var opts configFlags
	fs := flag.NewFlagSet("config show", flag.ContinueOnError)
	opts.bind(fs, false)
	if err := fs.Parse(args); err != nil {
		return err
	}
	doc, err := hostconfig.Load(hostconfig.ResolvePath(opts.path))
	if err != nil {
		return err
	}
	out, err := doc.Render()
	if err != nil {
		return err
	}
	if !doc.Existed() {
		fmt.Fprintf(os.Stderr, "› %s does not exist yet; every key is at capture-eye's built-in default\n", doc.Path)
	}
	fmt.Print(string(out))
	return nil
}

func configGet(args []string) error {
	var opts configFlags
	fs := flag.NewFlagSet("config get", flag.ContinueOnError)
	opts.bind(fs, false)
	if err := fs.Parse(args); err != nil {
		return err
	}
	if fs.NArg() != 1 {
		return errors.New("usage: horusctl config get KEY")
	}
	doc, err := hostconfig.Load(hostconfig.ResolvePath(opts.path))
	if err != nil {
		return err
	}
	value, err := doc.Get(fs.Arg(0))
	if err != nil {
		return err
	}
	fmt.Println(hostconfig.Format(value))
	return nil
}

func configSet(args []string) error {
	var opts configFlags
	fs := flag.NewFlagSet("config set", flag.ContinueOnError)
	opts.bind(fs, true)
	if err := fs.Parse(args); err != nil {
		return err
	}
	pairs := fs.Args()
	if len(pairs) == 0 || len(pairs)%2 != 0 {
		return errors.New("usage: horusctl config set KEY VALUE [KEY VALUE...]")
	}
	return withDoc(opts, func(doc *hostconfig.Doc) error {
		for i := 0; i < len(pairs); i += 2 {
			if err := doc.Set(pairs[i], hostconfig.ParseValue(pairs[i+1])); err != nil {
				return err
			}
		}
		return nil
	})
}

func configUnset(args []string) error {
	var opts configFlags
	fs := flag.NewFlagSet("config unset", flag.ContinueOnError)
	opts.bind(fs, true)
	if err := fs.Parse(args); err != nil {
		return err
	}
	if fs.NArg() == 0 {
		return errors.New("usage: horusctl config unset KEY [KEY...]")
	}
	return withDoc(opts, func(doc *hostconfig.Doc) error {
		for _, key := range fs.Args() {
			if err := doc.Unset(key); err != nil {
				return err
			}
		}
		return nil
	})
}

func configSetVideo(args []string) error {
	var opts configFlags
	fs := flag.NewFlagSet("config set-video", flag.ContinueOnError)
	opts.bind(fs, true)
	device := fs.String("device", "", "video device path, e.g. /dev/video0")
	size := fs.String("size", "", "capture resolution as WxH, e.g. 1280x720")
	fourcc := fs.String("fourcc", "", "pixel format, exactly 4 characters, e.g. MJPG")
	fps := fs.Int("fps", 0, "frame rate")
	decodeScale := fs.Int("decode-scale", 0, "JPEG decode downscale: 1, 2 or 4")
	flipH := fs.String("flip-h", "", "mirror horizontally: true|false")
	flipV := fs.String("flip-v", "", "flip vertically: true|false")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if fs.NFlag() == 0 {
		fs.Usage()
		return errors.New("set-video needs at least one flag")
	}
	return withDoc(opts, func(doc *hostconfig.Doc) error {
		if *device != "" {
			if err := doc.Set("capture.device", *device); err != nil {
				return err
			}
		}
		if *size != "" {
			width, height, err := parseSize(*size)
			if err != nil {
				return err
			}
			if err := doc.Set("capture.width", width); err != nil {
				return err
			}
			if err := doc.Set("capture.height", height); err != nil {
				return err
			}
		}
		if *fourcc != "" {
			// Length is checked here only to fail before the file is touched;
			// capture-eye validates it again on the way in regardless.
			if len(*fourcc) != 4 {
				return fmt.Errorf("--fourcc: expected 4 characters, got %q", *fourcc)
			}
			if err := doc.Set("capture.fourcc", *fourcc); err != nil {
				return err
			}
		}
		if *fps != 0 {
			if err := doc.Set("capture.fps", *fps); err != nil {
				return err
			}
		}
		if *decodeScale != 0 {
			if err := doc.Set("capture.decode_scale", *decodeScale); err != nil {
				return err
			}
		}
		if err := setBoolFlag(doc, "capture.flip_horizontal", *flipH); err != nil {
			return err
		}
		return setBoolFlag(doc, "capture.flip_vertical", *flipV)
	})
}

func configSetSerial(args []string) error {
	var opts configFlags
	fs := flag.NewFlagSet("config set-serial", flag.ContinueOnError)
	opts.bind(fs, true)
	port := fs.String("port", "", "serial device path (a /dev/serial/by-id/... path survives replugging)")
	baud := fs.Int("baud", 0, "baud rate")
	enabled := fs.String("enabled", "", "whether to open the serial port at all: true|false")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if fs.NFlag() == 0 {
		fs.Usage()
		return errors.New("set-serial needs at least one flag")
	}
	return withDoc(opts, func(doc *hostconfig.Doc) error {
		if *port != "" {
			if err := doc.Set("serial.port", *port); err != nil {
				return err
			}
		}
		if *baud != 0 {
			if err := doc.Set("serial.baud", *baud); err != nil {
				return err
			}
		}
		return setBoolFlag(doc, "serial.enabled", *enabled)
	})
}

// configEdit is the escape hatch for changes the typed flags don't cover. It
// still goes through the same validate-then-rename path, so $EDITOR cannot
// install a config that capture-eye would refuse to start with.
func configEdit(args []string) error {
	var opts configFlags
	fs := flag.NewFlagSet("config edit", flag.ContinueOnError)
	opts.bind(fs, true)
	if err := fs.Parse(args); err != nil {
		return err
	}
	path := hostconfig.ResolvePath(opts.path)
	doc, err := hostconfig.Load(path)
	if err != nil {
		return err
	}
	current, err := doc.Render()
	if err != nil {
		return err
	}

	tmp, err := os.CreateTemp("", "horusctl-*.json")
	if err != nil {
		return err
	}
	defer os.Remove(tmp.Name())
	if _, err := tmp.Write(current); err != nil {
		tmp.Close()
		return err
	}
	tmp.Close()

	editor := os.Getenv("EDITOR")
	if editor == "" {
		editor = "vi"
	}
	cmd := exec.Command(editor, tmp.Name())
	cmd.Stdin, cmd.Stdout, cmd.Stderr = os.Stdin, os.Stdout, os.Stderr
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("%s: %w", editor, err)
	}

	edited, err := hostconfig.Load(tmp.Name())
	if err != nil {
		return err
	}
	edited.Path = path
	if err := save(edited, opts); err != nil {
		return err
	}
	return nil
}

// configDevices answers the question every camera/serial change starts with:
// what is actually attached right now, and what does it support?
func configDevices(args []string) error {
	var opts configFlags
	fs := flag.NewFlagSet("config devices", flag.ContinueOnError)
	opts.bind(fs, false)
	device := fs.String("device", "", "also print the capture formats this camera supports")
	if err := fs.Parse(args); err != nil {
		return err
	}

	fmt.Println("cameras:")
	for _, path := range listCameras() {
		fmt.Printf("  %s\n", path)
	}

	fmt.Println("serial ports:")
	// The same Espressif auto-detect the TUI uses, so `devices` and a bare
	// `horusctl` never disagree about which board is the board.
	if port, err := link.AutoDetect(); err == nil {
		fmt.Printf("  %s (Espressif)\n", port)
	} else {
		fmt.Printf("  %v\n", err)
	}
	for _, path := range listByID("/dev/serial/by-id") {
		fmt.Printf("  %s\n", path)
	}

	if *device != "" {
		fmt.Printf("\nformats for %s:\n", *device)
		out, err := exec.Command(opts.bin, "--list-formats", "--device", *device).CombinedOutput()
		fmt.Print(string(out))
		if err != nil {
			return fmt.Errorf("%s --list-formats: %w", opts.bin, err)
		}
	}
	return nil
}

// listCameras prefers the stable by-id paths — a bare /dev/videoN is assigned
// in enumeration order and can move between boots, which is exactly the kind
// of breakage this CLI is meant to make easy to fix, not to cause.
func listCameras() []string {
	if paths := listByID("/dev/v4l/by-id"); len(paths) > 0 {
		return paths
	}
	matches, _ := filepath.Glob("/dev/video*")
	sort.Strings(matches)
	if len(matches) == 0 {
		return []string{"  (none found)"}
	}
	return matches
}

func listByID(dir string) []string {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil
	}
	var out []string
	for _, entry := range entries {
		out = append(out, filepath.Join(dir, entry.Name()))
	}
	sort.Strings(out)
	return out
}

// withDoc is the one write path every mutating subcommand goes through: load,
// apply, validate, atomically replace, optionally restart.
func withDoc(opts configFlags, apply func(*hostconfig.Doc) error) error {
	doc, err := hostconfig.Load(hostconfig.ResolvePath(opts.path))
	if err != nil {
		return err
	}
	if err := apply(doc); err != nil {
		return err
	}
	return save(doc, opts)
}

func save(doc *hostconfig.Doc, opts configFlags) error {
	created := !doc.Existed()
	if err := doc.Save(opts.bin); err != nil {
		if errors.Is(err, hostconfig.ErrNoBinary) {
			// Not fatal: a workstation editing a config destined for another
			// host has no capture-eye to validate with, and refusing to write
			// would be worse than writing with a warning.
			fmt.Fprintf(os.Stderr, "!! %v — writing without validation\n", err)
			if err := doc.SaveUnchecked(); err != nil {
				return err
			}
		} else {
			return err
		}
	}
	verb := "updated"
	if created {
		verb = "created"
	}
	fmt.Printf("› %s %s\n", verb, doc.Path)

	if !opts.restart {
		fmt.Println("› restart to apply: horusctl service restart")
		return nil
	}
	mgr := service.Manager{Runner: service.Exec{Stdout: os.Stdout, Stderr: os.Stderr}, Out: os.Stdout}
	return mgr.Restart()
}

func setBoolFlag(doc *hostconfig.Doc, key, text string) error {
	if text == "" {
		return nil
	}
	value, err := strconv.ParseBool(text)
	if err != nil {
		return fmt.Errorf("%s: expected true or false, got %q", key, text)
	}
	return doc.Set(key, value)
}

func parseSize(text string) (int, int, error) {
	width, height, ok := strings.Cut(text, "x")
	if !ok {
		return 0, 0, fmt.Errorf("--size: expected WxH, got %q", text)
	}
	w, err := strconv.Atoi(width)
	if err != nil {
		return 0, 0, fmt.Errorf("--size: %q is not a width", width)
	}
	h, err := strconv.Atoi(height)
	if err != nil {
		return 0, 0, fmt.Errorf("--size: %q is not a height", height)
	}
	return w, h, nil
}
