# tagadder
Expiremental tool that works with the HiBy R3 Pro media database on the device.

Worked at one time, but currently appears to broken.

To run it, you will need to modify your R3 Pro's firmware using [hiby-firmware-tools](https://github.com/SuperTaiyaki/hiby-firmware-tools). To build, make sure you clone submodules and run the script to get a copy of sqlite3, then:
```sh
mkdir build && cd build
cmake ..
make tagadder
```

In addition, it expects a `.sfn` font file with cjk support at `/font_cjk.sfn` in the SD card. Build one from [Unifont](https://unifoundry.com/unifont/) using [sfnconv](https://gitlab.com/bztsrc/scalable-font/-/tree/master/sfnconv).
