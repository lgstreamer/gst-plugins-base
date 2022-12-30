LG webOS 5.x GStreamer - Base Plugins
=====================================

## Description

This directory contains the gst-plugins-base source, as compiled by LG to be
included in webOS 5.x devices, such as **LG CX OLED TVs**.

Thus, unless LG applied some changes that have not yet been published, the
binaries produced by compiling the source from this repository should work
as a drop-in replacement for the GStreamer binaries that are officially used
by LG in their CX OLED Smart TVs.

This can be very useful if you have [rooted your TV](https://github.com/RootMyTV/RootMyTV.github.io/issues/85#issuecomment-1295058979)
and want to alter this source to enable or restore functionality that is
not provided by default on your CX model.

## Origin

This source, which is licensed under LPGL v2.0, was obtained through a legal
inquiry at https://opensource.lge.com/inquiry and was extracted from the
`webOS 5.0 JO 2.0` archive that can be downloaded [here](http://opensource.lge.com/product/list?page=&ctgr=005&subCtgr=006&keyword=OLED65CX5LB).

The changes that have been applied by LG on top of the official GStreamer
1.14.4 source can be found in [this commit](https://github.com/lgstreamer/gst-plugins-base/commit/c7b820bb5a2820d9c3f83f582713be1b0e5b56f1).

## Compilation

### Toolchain installation

You will need a recent Linux system, with some GTK related system updates as
well as the webosbrew toolchain from https://www.webosbrew.org. On Debian 11,
the toolchain can be installed as follows:

```
apt install cmake doxygen libglib2.0-dev-bin gobject-introspection libgirepository1.0-dev
wget https://github.com/webosbrew/meta-lg-webos-ndk/releases/download/1.0.g-rev.5/webos-sdk-x86_64-armv7a-neon-toolchain-1.0.g.sh
chmod 755 webos-sdk-x86_64-armv7a-neon-toolchain-1.0.g.sh
./webos-sdk-x86_64-armv7a-neon-toolchain-1.0.g.sh
```

Note that, if using the toolchain above, you should also have compiled and
installed the GStreamer software from https://github.com/lgstreamer/gstreamer
otherwise the process will complain that your version of GStreamer is too old.

### Build process

Once the toolchain and base GStreamer have been installed, you can compile
and install the LG version of the base plugins (which you will need to do
in order to compile addtional GStreamer plugins) by issuing:

```
git clone https://github.com/lgstreamer/gst-plugins-base.git
cd gst-plugins-base
. /opt/webos-sdk-x86_64/1.0.g/environment-setup-armv7a-neon-webos-linux-gnueabi
./autogen.sh --noconfigure
patch -p1 < gst-plugins-base-1.14.4-make43.patch
# NB, you *MUST* re-run autogen.sh here rather than invoke configure or else the build will fail
./autogen.sh --host=arm-webos-linux-gnueabi --with-sysroot=${SDKTARGETSYSROOT} \
  --prefix=${SDKTARGETSYSROOT}/usr/ \
  --disable-silent-rules --disable-dependency-tracking --disable-gtk-doc \
  --disable-introspection --disable-examples --disable-static \
  --enable-alsa --disable-cdparanoia --disable-debug \
  --disable-dispmanx --enable-egl --disable-gbm \
  --disable-gio_unix_2_0 --enable-gles2 --enable-ivorbis \
  --disable-jpeg --enable-ogg --disable-opengl \
  --disable-opus --disable-orc --disable-pango \
  --disable-png --disable-theora --disable-valgrind \
  --disable-libvisual --enable-vorbis --enable-wayland \
  --disable-x --disable-xvideo --disable-xshm --enable-zlib --enable-nls
./fix_sysroot.sh
make -j6 install
```
