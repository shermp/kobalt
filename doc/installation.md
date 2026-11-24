# Installation instructions

### Install on Kobo

> [!IMPORTANT]
> Keep a backup of your books and reading data before testing, no matter whether you use the website or not. I have done my best to make Kobalt as stable and safe as possible, but you should always have a backup, just to be safe.

#### Automatic installation via Chrome

The easiest way is via [the website](https://kobalt.nicoverbruggen.be). Click on **Install or remove on Kobo** and you should follow the steps on the website. The website will check if your device is compatible.

#### Manual installation

There are two installation packages: one for the firmware 4.x line, and one for the latest beta, which is currently at version 6.x. Choose the right one for your Kobo, so check what software version you have installed first!

| Package | Firmware
|---|---|
| `Kobalt-qt5.zip` | 4.38.21908 and later 4.x (tested up to 4.46)
| `Kobalt-qt6.zip` | 5.18 and later 5.x, and 6.x (tested up to 6.0.2)

_(*) Due to how software updates work on Kobo OS 6, you may need a future version of [NickelModManager](https://github.com/nicoverbruggen/NickelModManager) to avoid needing to reinstall mods._

1. Connect your Kobo over USB and extract the ZIP package on your computer. Show hidden files so you can see `.adds` and `.kobo`.
2. Copy the complete `.adds` folder to the root of the Kobo drive. Merge it with the existing folder and replace matching package files. Keep other apps, settings and reading data.
3. Copy the complete `.kobo` folder from the extracted package to the same drive root. Merge it with the existing folder. Firmware 4 uses `.kobo/KoboRoot.tgz`; firmware 5 and 6 use `.kobo/Kobo.tgz`.
4. Eject the Kobo safely, disconnect USB and let it install the update.

Copy every file supplied in the ZIP, keeping its folder path. Copy `.adds` before `.kobo` so the assets are present when the update starts. Do not copy only the `.tgz` file or unpack it yourself. Kobalt creates its support files and installation marker on first startup.

### Removing Kobalt from Kobo devices

#### Automatic removal via Chrome

Like the installation, the easiest way is via [the website](https://kobalt.nicoverbruggen.be). Click on **Install or remove on Kobo** and you should follow the steps on the website. Removal is the easiest this way.

#### Manual removal

You can also connect your device via USB and remove the uninstallation file (`.adds/kobalt/uninstall`) which will cause Kobalt to remove itself but retain your configuration files, annotations and more. If you would like to remove _everything_, remove `.adds/kobalt` in its entirety. After removing either of these, make sure to safely eject your device.
