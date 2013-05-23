# tilesrv - simple fast-cgi tile server for deepzoom

This is a tiny fastcgi DeepZoom tile server for image files which can be read 
by [openslide](http://openslide.org/). 

If you need viewing to be quick you should process your slides with a
proper DeepZoom converter. These programs take a virtual slide and create a
large pyramid of jpeg tiles which can then be sent by any webserver to the
viewer.

This conversion can be slow, perhaps 10 minutes per slide. If you have
many thousands of slides and not many users it might be easier to create the
jpeg tiles on the fly as viewers request them. That's what this program does.

It takes about 2s to open and prepare a typical image file, but then viewing
is relatively quick. You should be able to support up to about 10 
simultaneous users on a basic server. 

Openslide have a program in Python which does this same task, see the
examples/ directory in openslide-python. It does not include a fastcgi
interface though. 

## Dependencies

```bash
apt-get install libfcgi-dev liburiparser-dev libvips-dev
```

This program needs libvips 7.32.3 or later. You may need to build
from source if your distribution does not have it. 

## Install

Compile with:

```bash
gcc -g -Wall tilesrv.c \
	`pkg-config vips --cflags --libs` \
	-lfcgi -luriparser \
	-o tilesrv.fcgi
```

Copy to:

/usr/local/httpd/fcgi-bin

You may need to create the logfile, eg.

```bash
touch /tmp/tilesrv.log
chmod ugo+rw /tmp/tilesrv.log
```

Sample fcgi.conf:

```xml
<IfModule mod_fastcgi.c>
  FastCgiIpcDir /var/lib/apache2/fastcgi

  # Create a directory for the tilesrv binary
  ScriptAlias /fcgi-bin/ "/usr/local/httpd/fcgi-bin/"

  # Set the options on that directory
  <Directory "/usr/local/httpd/fcgi-bin">
     AllowOverride None
     Options None
     Order allow,deny
     Allow from all
  </Directory>

  # Set the module handler
  AddHandler fastcgi-script fcg fcgi fpl

  # Initialise some variables for the FCGI server
  FastCgiServer /usr/local/httpd/fcgi-bin/tilesrv.fcgi \
    -initial-env LOGFILE=/tmp/tilesrv.log 
  
</IfModule>
```

Then test with a URL like:

```
http://localhost/fcgi-bin/tilesrv.fcgi?filename=/home/john/pics/044.svs&path=12/2_0.jpg
```

You'll need to use something like
[mod_rewrite](http://httpd.apache.org/docs/current/mod/mod_rewrite.html) to
make those from DeepZoom tile references, which are usually something like:

```
http://localhost/pics/044_files/12/2_0.jpg
```

You'll also need to make a .dzi file for each image. You can use a shell script
to do this, something like:

```bash
#!/bin/bash

for i in $*; do
  out=$(dirname $i)/$(basename $i .svs).dzi

  width=$(header $i -f Xsize)
  height=$(header $i -f Ysize)

  cat >$out << EOF
<?xml version="1.0" encoding="UTF-8"?>
<Image
  xmlns="http://schemas.microsoft.com/deepzoom/2008"
  Format="jpeg"
  Overlap="0"
  TileSize="256"
  >
  <Size
    Height="$width"
    Width="$height"
  />
</Image>
EOF
done
```

## Cross-compiling

Cross-compile from linux to Windows with:

```bash
i686-w64-mingw32-gcc \
	-mms-bitfields -march=i686 \
	-I/home/john/GIT/build-win32/7.32/inst/include \
	-I/home/john/GIT/build-win32/7.32/inst/include/glib-2.0 \
	-I/home/john/GIT/build-win32/7.32/inst/lib/glib-2.0/include \
	tilesrv.c \
	-L/home/john/GIT/build-win32/7.32/inst/lib \
	-lvips -lz -ljpeg -lstdc++ -lxml2 -lfftw3 -lm -lMagickWand -llcms2 \
	-lopenslide -lcfitsio -lpangoft2-1.0 -ltiff -lpng14 -lexif \
	-lMagickCore -lpango-1.0 -lfreetype -lfontconfig -lgobject-2.0 \
	-lgmodule-2.0 -lgthread-2.0 -lglib-2.0 -lintl \
	-lfcgi -luriparser \
	-o tilesrv.fcgi
```

Or that's what seems to work for me, anyway. 
