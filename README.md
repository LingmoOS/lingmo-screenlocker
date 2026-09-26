# Screen locker

## Third Party Code

kcheckpass

## Dependencies

### Arch Linux

```
sudo pacman -S --needed cmake ninja qt6-base qt6-declarative qt6-tools qt6-5compat pam libx11 libxcb
```

Runtime: `lingmoui` (LingmoUI.CompatibleModule), `lib_lingmo` (Lingmo.Accounts/System/Mpris QML
modules) and a PAM service named `kde` (`/usr/lib/pam.d/kde`, shipped by `kscreenlocker`), or pass
`-DKDE4_COMMON_PAM_SERVICE=<service>` at configure time (default: `lingmo-screenlocker`, whose PAM file is installed to `/etc/pam.d`).

### Debian/Ubuntu

```
sudo apt install libpam0g-dev libx11-dev -y
```

## Build

```shell
mkdir build
cd build
cmake -DCMAKE_INSTALL_PREFIX:PATH=/usr ..
make
```

## Install

```shell
sudo make install
```

## License

This project has been licensed by GPLv3.
