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
#     correct; `repoPath` must point at a real checkout. Both fail loudly
#     (an eval error, or capture-eye's own config errors) rather than guess.
{ config, lib, pkgs, ... }:

let
  cfg = config.services.horus;
  webDist = "/var/lib/horus-web/dist";
  serverBin = "/var/lib/horus-server/bin/horus-server";
  goCache = "/var/cache/horus-go";
  runtimeDir = "horus";
  controlSocket = "/run/${runtimeDir}/control.sock";

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

    repoPath = lib.mkOption {
      type = lib.types.path;
      description = ''
        Path to a checkout of this repo on the target machine (needs web/).
        No default — a wrong guess here is worse than an eval error.
      '';
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
  };

  config = lib.mkIf cfg.enable {
    # --- device access: video capture, the serial port, VAAPI ---
    users.groups.horus = { };
    systemd.tmpfiles.rules = [
      "d ${webDist} 0755 root root -"
      "d /etc/horus 0755 root root -"
      # Data dir is service-owned (replay.json lives here); the bin/
      # subdirectory holding the built binary stays root-owned, so a
      # compromised horus-server process cannot rewrite its own executable.
      "d /var/lib/horus-server 0750 horus-server horus-server -"
      "d /var/lib/horus-server/bin 0755 root root -"
      "d ${goCache} 0755 root root -"
    ];

    # Lets a non-root capture-eye service (and any relay client in the
    # "horus" group) use the camera and the ESP32's CDC port without udev
    # rules the user has to write themselves.
    services.udev.extraRules = ''
      SUBSYSTEM=="video4linux", GROUP="video", MODE="0660"
      SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", GROUP="dialout", MODE="0660"
    '';

    # --- capture-eye: owns the camera, the serial port, and the relay socket ---
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
        SupplementaryGroups = [ "video" "dialout" "render" ];
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
      deps = [ "specialfs" ];
      text = ''
        export PATH=${pkgs.go}/bin:${pkgs.bash}/bin:$PATH
        export HOME=${goCache}
        export GOCACHE=${goCache}/build
        export GOPATH=${goCache}/path
        cd ${cfg.repoPath}/server
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
          + lib.optionalString (cfg.replayFile != null) " --replay ${cfg.replayFile}";
        Restart = "always";
        RestartSec = 2;
        User = "horus-server";
        Group = "horus-server";
        SupplementaryGroups = [ "horus" ]; # to open the relay socket capture-eye owns
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
      deps = [ "specialfs" ];
      text = ''
        export PATH=${pkgs.nodejs_22}/bin:${pkgs.bash}/bin:$PATH
        cd ${cfg.repoPath}/web
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
