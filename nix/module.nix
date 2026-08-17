# NixOS module for the whole horus-33 host stack: capture-eye (vision +
# control relay), horus-server (REST/WebSocket API), MediaMTX (WHEP/RTSP), and
# horus-web (static SPA) behind nginx on one port.
#
# Design choices this module makes concrete (see the plan for the reasoning):
#   - capture-eye takes a config file *path* only — never rendered by Nix.
#     Retuning the camera is `$EDITOR` + `systemctl restart`, not a rebuild.
#   - horus-web (npm) and horus-server (go) are both built by activation
#     scripts, not Nix derivations — mirrors stealth-operation/nix/modules/
#     client.nix. Neither has a reproducibility requirement that would justify
#     hand-maintaining an npmDepsHash/vendorHash on every dependency bump, so
#     both just rebuild from the checked-out repo on every activation,
#     unconditionally — capture-eye is the one piece where reproducibility
#     (the pinned model hash) actually matters, and it stays a real derivation.
#   - Nothing machine-specific has a default. `configFile` must exist and be
#     correct. `repoUrl` has no default either — a wrong guess is worse than
#     an eval error.
#   - horus-server/horus-web are built from a checkout the module clones and
#     updates *itself* (system.activationScripts.horusRepoSync, `git clone`/
#     `git fetch` against `repoUrl`/`repoRef`) rather than one you maintain by
#     hand — same "no hash, just always run" reasoning as the build steps
#     below: activationScripts run outside the Nix sandbox with real network
#     access, exactly like `npm ci`/`go build` already have. This keeps
#     horus-33 a separate flake/repo (never folded into the consuming flake's
#     own tree) while still needing zero manual `git clone` on the target
#     machine — point `repoUrl` at this repo and the module does the rest.
#   - `clipsDir` has the same dual-source-of-truth tradeoff as `configFile`:
#     Nix creates the directory and tells horus-server about it, but capture-
#     eye only writes clips there if the user's own configFile JSON sets
#     clipping.output_dir (and clipping.admin_socket_path, for live toggling)
#     to match — Nix cannot enforce that consistency across a file it doesn't
#     render.
{ config, lib, pkgs, ... }:

let
  cfg = config.services.horus;
  webDist = "/var/lib/horus-web/dist";
  serverBin = "/var/lib/horus-server/bin/horus-server";
  goCache = "/var/cache/horus-go";
  repoDir = "/opt/horus-33";
  runtimeDir = "horus";
  controlSocket = "/run/${runtimeDir}/control.sock";
  clipAdminSocket = "/run/${runtimeDir}/clip-admin.sock";

  capturePkg = pkgs.callPackage ../nix/capture-eye.nix { };

  mediamtxConfig = pkgs.writers.writeYAML "mediamtx.yml" {
    logLevel = "info";

    rtsp = true;
    rtspTransports = [ "tcp" ];
    rtspAddress = ":8554";

    webrtc = true;
    webrtcAddress = ":8889";
    # Both candidate types: UDP for LAN (best quality), TCP so a single-port
    # or tunnelled deployment (e.g. behind Cloudflare) still works — neither
    # forwards the other's traffic. See the plan's M10 "Note on UDP".
    webrtcLocalUDPAddress = ":8189";
    webrtcLocalTCPAddress = ":8189";
    # No third-party ICE server ever sees the stream — LAN/loopback only.
    webrtcICEServers2 = [ ];

    hls = false;
    rtmp = false;
    srt = false;

    paths.eye.source = "publisher";
  };
in
{
  options.services.horus = {
    enable = lib.mkEnableOption "the horus-33 host stack (capture-eye, horus-server, MediaMTX, horus-web)";

    configFile = lib.mkOption {
      type = lib.types.path;
      default = "/etc/horus/capture-eye.json";
      description = ''
        Path to capture-eye's JSON config file (see capture-eye/docs/config.md).
        Passed straight through as --config; Nix never renders or validates
        this file's contents. Point it at a pkgs.writers.writeJSON-generated
        path instead if you want Nix to manage it after all.
      '';
    };

    repoUrl = lib.mkOption {
      type = lib.types.str;
      description = ''
        Git URL of this repo (e.g. "https://github.com/canavan-a/horus-33.git"
        or a local "file:///..." path). The module clones/updates it itself into
        ${repoDir} on every activation — no manual checkout to maintain, and
        no vendorHash/npmDepsHash, same tradeoff the build steps already make.
        No default — a wrong guess here is worse than an eval error.
      '';
    };

    repoRef = lib.mkOption {
      type = lib.types.str;
      default = "main";
      description = "Branch, tag, or commit to build from.";
    };

    listenAddress = lib.mkOption {
      type = lib.types.str;
      default = "0.0.0.0";
      description = "Address nginx binds for the web UI and API. \"127.0.0.1\" for loopback-only.";
    };

    listenPort = lib.mkOption {
      type = lib.types.port;
      default = 33;
      description = "Port nginx binds for the web UI and API.";
    };

    openFirewall = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = "Open listenPort (TCP) and the WebRTC ICE port (8189, UDP+TCP).";
    };

    replayFile = lib.mkOption {
      type = lib.types.nullOr lib.types.path;
      default = "/var/lib/horus-server/replay.json";
      description = ''
        Where horus-server persists last-known control values to replay on the
        device's next hello (the firmware has no persistence of its own).
        Set to null to disable replay.
      '';
    };

    clipsDir = lib.mkOption {
      type = lib.types.path;
      default = "/var/lib/horus-capture-eye/clips";
      description = ''
        Directory capture-eye writes finished clips to and horus-server serves
        them from. Nix only creates this directory and passes its path to
        horus-server via --clips-dir; capture-eye itself only writes here if
        your configFile's "clipping.output_dir" key is set to this same path
        — Nix cannot enforce that consistency, the same tradeoff configFile
        itself already makes (see the module's top comment).
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    # --- device access: video capture, the serial port, VAAPI ---
    users.groups.horus = { };
    # Narrowly scoped to just clip files, same one-group-per-boundary pattern
    # as "horus" above (relay socket) and "horus-server" below (its own data
    # dir) — capture-eye has no dedicated User= (runs effectively as root, for
    # stable /dev/video*, /dev/ttyACM0, /dev/dri access), so the only way for
    # horus-server's non-root user to read clips capture-eye writes is via a
    # shared supplementary group.
    users.groups.horus-clips = { };
    systemd.tmpfiles.rules = [
      "d ${webDist} 0755 root root -"
      "d /etc/horus 0755 root root -"
      # Data dir is service-owned (replay.json lives here); the bin/
      # subdirectory holding the built binary stays root-owned, so a
      # compromised horus-server process cannot rewrite its own executable.
      "d /var/lib/horus-server 0750 horus-server horus-server -"
      "d /var/lib/horus-server/bin 0755 root root -"
      "d ${goCache} 0755 root root -"
      # setgid (leading 2): every file capture-eye (root-ish) creates here
      # inherits group horus-clips regardless of capture-eye's own primary
      # group, so horus-server's read access doesn't depend on capture-eye
      # remembering to chgrp anything itself.
      "d ${cfg.clipsDir} 2750 root horus-clips -"
      # git clone creates ${repoDir} itself; only its parent needs to exist
      # first, and /opt isn't guaranteed to already be there on NixOS.
      "d /opt 0755 root root -"
    ];

    # Clones (first activation) or fast-forwards (every activation after)
    # ${repoDir} from repoUrl/repoRef — this is what lets horus-server/
    # horus-web build below without anyone maintaining a checkout by hand.
    # Runs before both build steps (their `deps`), and — like them — outside
    # the Nix sandbox, so real network access here is expected, not a hack.
    system.activationScripts.horusRepoSync = {
      deps = [ "specialfs" ];
      text = ''
        export PATH=${pkgs.git}/bin:${pkgs.bash}/bin:$PATH
        if [ ! -d ${repoDir}/.git ]; then
          git clone ${cfg.repoUrl} ${repoDir}
        fi
        # fetch+checkout FETCH_HEAD (not `pull`/`--branch`) so repoRef can be
        # a branch, a tag, or a bare commit SHA — all fetch the same way.
        git -C ${repoDir} fetch origin ${cfg.repoRef}
        git -C ${repoDir} checkout --force FETCH_HEAD
      '';
    };

    # Lets a non-root capture-eye service (and any relay client in the
    # "horus" group) use the camera and the ESP32's CDC port without udev
    # rules the user has to write themselves.
    services.udev.extraRules = ''
      SUBSYSTEM=="video4linux", GROUP="video", MODE="0660"
      SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", GROUP="dialout", MODE="0660"
    '';

    # --- capture-eye: owns the camera, the serial port, and the relay socket ---
    # `systemctl status horus-capture-eye` shows the exit code straight in
    # "code=exited, status=N" — capture-eye/docs/config.md's "Exit codes"
    # table decodes it without opening the journal: 10 = no camera / camera
    # rejected the request, 11 = no ESP32 / serial link failed to open,
    # 2 = bad config, 1 = anything else. Restart=always + RestartSec=2 below
    # keep relaunching it regardless of which one it was — this only makes
    # *why* visible at a glance, it doesn't change what's fatal or how fast
    # it retries.
    systemd.services.horus-capture-eye = {
      description = "horus-33 vision pipeline (capture, inference, tracking, control relay)";
      after = [ "horus-mediamtx.service" "network.target" ];
      wants = [ "horus-mediamtx.service" ];
      wantedBy = [ "multi-user.target" ];

      serviceConfig = {
        ExecStart = "${capturePkg}/bin/capture-eye --config ${cfg.configFile}";
        Restart = "always";
        RestartSec = 2;
        # capture-eye currently exits outright when a frame sink fails at
        # startup (e.g. MediaMTX not up yet) — Restart=always is what makes
        # that self-heal rather than requiring manual intervention.
        DynamicUser = false; # needs stable access to /dev/video*, /dev/ttyACM0, /dev/dri
        SupplementaryGroups = [ "video" "dialout" "render" "horus-clips" ];
        RuntimeDirectory = runtimeDir;
        RuntimeDirectoryMode = "0770";
        RuntimeDirectoryPreserve = true;
      };
    };

    # --- horus-server: proxies REST/WebSocket to the relay socket ---
    users.users.horus-server = {
      isSystemUser = true;
      group = "horus-server";
      extraGroups = [ "horus" ];
    };
    users.groups.horus-server = { };

    # Built by an activation script, same reasoning and same pattern as
    # horus-web below: a Go vendorHash kept in sync by hand on every go.sum
    # change is the same maintenance cost as npm's npmDepsHash, for a binary
    # with no reproducibility requirement that would justify carrying it.
    # goCache persists the module/build cache across rebuilds so this stays
    # fast after the first run; only go.sum changes trigger real downloads.
    system.activationScripts.horusServerBuild = {
      deps = [ "horusRepoSync" ];
      text = ''
        export PATH=${pkgs.go}/bin:${pkgs.bash}/bin:$PATH
        export HOME=${goCache}
        export GOCACHE=${goCache}/build
        export GOPATH=${goCache}/path
        cd ${repoDir}/server
        ${pkgs.go}/bin/go build -o ${serverBin}.new ./cmd/horus-server
        mv -f ${serverBin}.new ${serverBin}
        ${pkgs.systemd}/bin/systemctl try-restart horus-server.service || true
      '';
    };

    systemd.services.horus-server = {
      description = "horus-33 REST + WebSocket API";
      after = [ "horus-capture-eye.service" ];
      # A hint, not a hard dependency: horus-server retries the relay socket
      # on its own (internal/link/unix.go's reconnect loop), so it does not
      # need capture-eye to have started first, only eventually.
      wants = [ "horus-capture-eye.service" ];
      wantedBy = [ "multi-user.target" ];

      serviceConfig = {
        ExecStart = "${serverBin}"
          + " --socket ${controlSocket}"
          + " --listen 127.0.0.1:8090"
          + " --clips-dir ${cfg.clipsDir}"
          + " --clip-admin-socket ${clipAdminSocket}"
          + lib.optionalString (cfg.replayFile != null) " --replay ${cfg.replayFile}";
        Restart = "always";
        RestartSec = 2;
        User = "horus-server";
        Group = "horus-server";
        SupplementaryGroups = [ "horus" "horus-clips" ]; # relay socket + clip files, both owned by capture-eye
      };
    };

    # --- MediaMTX: RTSP ingest from capture-eye, WHEP egress to browsers ---
    systemd.services.horus-mediamtx = {
      description = "horus-33 media server (RTSP in, WHEP out)";
      wantedBy = [ "multi-user.target" ];
      serviceConfig = {
        ExecStart = "${pkgs.mediamtx}/bin/mediamtx ${mediamtxConfig}";
        Restart = "always";
        RestartSec = 2;
        DynamicUser = true;
      };
    };

    # --- horus-web: built fresh on every activation, not a Nix derivation ---
    # See the plan's M10 "web/ is built by an activation script" for why: no
    # npmDepsHash to keep in sync, and it can never silently go stale. Costs a
    # `npm ci && npm run build` on every `nixos-rebuild switch`/boot.
    system.activationScripts.horusWebBuild = {
      deps = [ "horusRepoSync" ];
      text = ''
        export PATH=${pkgs.nodejs_22}/bin:${pkgs.bash}/bin:$PATH
        cd ${repoDir}/web
        ${pkgs.nodejs_22}/bin/npm ci
        ${pkgs.nodejs_22}/bin/npm run build
        rm -rf ${webDist}/*
        cp -r dist/* ${webDist}/
        ${pkgs.systemd}/bin/systemctl try-reload-or-restart nginx.service || true
      '';
    };

    # --- nginx: the one port anything outside this host needs to reach ---
    services.nginx = {
      enable = true;
      recommendedProxySettings = true;
      recommendedGzipSettings = true;

      virtualHosts.horus = {
        listenAddresses = [ cfg.listenAddress ];
        listen = [{ addr = cfg.listenAddress; port = cfg.listenPort; }];

        root = webDist;
        locations."/" = { tryFiles = "$uri /index.html"; };

        locations."/api/" = {
          proxyPass = "http://127.0.0.1:8090/api/";
          proxyWebsockets = true; # required for /api/ws
        };

        # Public shape is /whep/eye; MediaMTX itself wants /eye/whep — matches
        # web/vite.config.ts's dev-proxy rewrite and web/VideoPanel.tsx's
        # WHEP_URL exactly, so the client behaves identically in dev and in
        # production. Hardcoded to the one stream capture-eye/mediamtx.yml
        # actually publishes (path "eye") rather than a generic regex capture
        # — nginx's build-time gixy check (part of NixOS's nginx module)
        # flags a captured variable interpolated into proxy_pass as a
        # possible HTTP-splitting vector, and there is no second stream today
        # that would justify carrying that risk for genericity nothing uses.
        locations."/whep/eye" = {
          proxyPass = "http://127.0.0.1:8889/eye/whep";
        };
      };
    };

    networking.firewall = lib.mkIf cfg.openFirewall {
      allowedTCPPorts = [ cfg.listenPort 8189 ];
      allowedUDPPorts = [ 8189 ];
    };
  };
}
