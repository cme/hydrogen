#!/usr/bin/env python3

import os

cellar = None
for x in [ "/usr/local/Cellar", "/opt/homebrew/Cellar" ]:
    if os.path.isdir(x):
        cellar = x

if cellar == None:
    print("Cannot find Cellar")
    exit()

stamps = {}
for b in os.listdir(cellar):
    stamps[b] = os.path.getmtime(os.path.join(cellar, b))

# Install stuff.
if True:
    os.system("      brew update")
    # Build libsndfile and bdb from source to enable building for 10.12
    os.system("      brew install --build-from-source ./macos/HomebrewFormulae/berkeley-db.rb")
    os.system("      brew install --build-from-source ./macos/HomebrewFormulae/libogg.rb")
    os.system("      brew install --build-from-source ./macos/HomebrewFormulae/libvorbis.rb")
    os.system("      brew install --build-from-source ./macos/HomebrewFormulae/libsndfile.rb")

    os.system("     brew install qt5; export CMAKE_PREFIX_PATH=\"$(brew --prefix qt5)\";")
    os.system("     brew install libarchive; export PKG_CONFIG_PATH=\"$(brew --prefix libarchive)/lib/pkgconfig\";")
    os.system("     brew install libsndfile jack pulseaudio cppunit")


for b in os.listdir(cellar):
    p = os.path.join(cellar, b)
    if not b in stamps or stamps[b] != os.path.getmtime(p):
        print("updated or new: %s" % p)
        os.system("tar czf %s.tar.gz %s" % (b, p))
        os.system("ls -alrt %s.tar.gz" % b)
    else:
        print("%s not updated" % b)
