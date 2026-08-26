use crate::{build, command, version};
use alvr_filesystem as afs;
use std::path::PathBuf;
use xshell::{cmd, Shell};

fn build_windows_installer() {
    let sh = Shell::new().unwrap();

    let wix_path = PathBuf::from(r"C:\Program Files (x86)\WiX Toolset v3.11\bin");
    let heat_cmd = wix_path.join("heat.exe");
    let candle_cmd = wix_path.join("candle.exe");
    let light_cmd = wix_path.join("light.exe");

    // Clear away build and prerelease version specifiers, MSI can have only dot-separated numbers.
    let mut version = version::version();
    if let Some(idx) = version.find('-') {
        version = version[..idx].to_owned();
    }
    if let Some(idx) = version.find('+') {
        version = version[..idx].to_owned();
    }

    let server_build_dir = afs::server_build_dir();
    let wix_source_dir = afs::crate_dir("xtask").join("wix");
    let wix_target_dir = afs::target_dir().join("wix");
    let main_source = wix_source_dir.join("main.wxs");
    let main_object = wix_target_dir.join("main.wixobj");
    let harvested_source = wix_target_dir.join("harvested.wxs");
    let harvested_object = wix_target_dir.join("harvested.wixobj");
    let alvr_msi = wix_target_dir.join("alvr.msi");
    let bundle_source = wix_source_dir.join("bundle.wxs");
    let bundle_object = wix_target_dir.join("bundle.wixobj");
    let installer = afs::build_dir().join(format!("ALVR_Installer_v{version}.exe"));

    cmd!(sh, "{heat_cmd} dir {server_build_dir} -ag -sreg -srd -dr APPLICATIONFOLDER -cg BuildFiles -var var.BuildRoot -o {harvested_source}").run().unwrap();
    cmd!(sh, "{candle_cmd} -arch x64 -dBuildRoot={server_build_dir} -ext WixUtilExtension -dVersion={version} {main_source} {harvested_source} -o {wix_target_dir}\\").run().unwrap();
    cmd!(sh, "{light_cmd} {main_object} {harvested_object} -ext WixUIExtension -ext WixUtilExtension -o {alvr_msi}").run().unwrap();
    cmd!(sh, "{candle_cmd} -arch x64 -dBuildRoot={server_build_dir} -ext WixUtilExtension -ext WixBalExtension {bundle_source} -o {wix_target_dir}\\").run().unwrap();
    cmd!(
        sh,
        "{light_cmd} {bundle_object} -ext WixUtilExtension -ext WixBalExtension -o {installer}"
    )
    .run()
    .unwrap();
}

pub fn package_server(root: Option<String>, gpl: bool) {
    let sh = Shell::new().unwrap();

    build::build_server(true, gpl, root, true, false);

    // Add licenses
    let licenses_dir = afs::server_build_dir().join("licenses");
    sh.create_dir(&licenses_dir).unwrap();
    sh.copy_file(
        afs::workspace_dir().join("LICENSE"),
        licenses_dir.join("ALVR.txt"),
    )
    .unwrap();
    sh.copy_file(
        afs::crate_dir("server").join("LICENSE-Valve"),
        licenses_dir.join("Valve.txt"),
    )
    .unwrap();
    if gpl {
        sh.copy_file(
            afs::deps_dir().join("windows/ffmpeg/LICENSE.txt"),
            licenses_dir.join("FFmpeg.txt"),
        )
        .ok();
    }

    // Gather licenses with cargo about. Non-fatal: the tool's dependency tree
    // no longer resolves cleanly with modern toolchains, and the dashboard
    // license page is cosmetic.
    let licenses_template = afs::crate_dir("xtask").join("licenses_template.hbs");
    let install_ok = std::process::Command::new("cargo")
        .args(["+stable", "install", "cargo-about"])
        .status()
        .map(|s| s.success())
        .unwrap_or(false);
    let licenses_content = if install_ok {
        cmd!(sh, "cargo about generate {licenses_template}")
            .read()
            .unwrap_or_default()
    } else {
        String::new()
    };
    sh.write_file(licenses_dir.join("dependencies.html"), licenses_content)
        .unwrap();

    // Finally package everything
    if cfg!(windows) {
        command::zip(&sh, &afs::server_build_dir()).unwrap();

        sh.copy_file(
            afs::target_dir().join("release").join("alvr_server.pdb"),
            afs::build_dir(),
        )
        .unwrap();

        // Installer is optional: WiX may be unavailable on CI; the portable zip is the primary artifact.
        if PathBuf::from(r"C:\Program Files (x86)\WiX Toolset v3.11\bin\heat.exe").exists() {
            build_windows_installer();
        } else {
            println!("WiX not found, skipping installer");
        }
    } else {
        command::targz(&sh, &afs::server_build_dir()).unwrap();
    }
}

pub fn package_client_lib() {
    let sh = Shell::new().unwrap();

    build::build_client_lib(true);

    command::zip(&sh, &afs::build_dir().join("alvr_client_core")).unwrap();
}
