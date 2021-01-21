# rgb2yuv
Stash of some format-conversion code in case I need it later.
The code converts an RGB-format image to a YUV-format in a Direct3D 12 compute shader, RgbToYuvCS.hlsl.

# Usage
The usage is
```
Rgb2Yuv.exe [sourceImageFile]
```
The file "sourceImageFile" is loaded by WIC so common formats (.jpg, .png) are supported.

The program exits after doing the conversion. The idea is to easily drop-in the code (or just the shader) in an existing system, rather than use it by itself. It could be extended to display or save out without too much trouble though.

If the program is run under Pix for Windows the conversion gets captured by Pix. I validated stuff that way.
