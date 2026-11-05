if [ -x usr/bin/update-desktop-database ]; then
  chroot . usr/bin/update-desktop-database -q usr/share/applications >/dev/null 2>&1
fi
if [ -e usr/share/icons/hicolor/icon-theme.cache ] && [ -x usr/bin/gtk-update-icon-cache ]; then
  chroot . usr/bin/gtk-update-icon-cache -q -f usr/share/icons/hicolor >/dev/null 2>&1
fi
