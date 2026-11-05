# Xbox 360 shaders from HLSL, with XNA Game Studio 4.0

XNA Game Studio 4.0 compiled effects for the Xbox 360 on a PC, with
Microsoft's Xbox 360 shader compiler (the XDK's microcode compiler, inside
`XnaNative.dll`). No console is needed. XenoForge uses it to make shaders the
way that compiler makes them, from HLSL written to cover what the games seen
so far do not use (`tests/hlsl/`).

Nothing of XNA is in this repository. To set it up (no installation, no
Visual Studio 2010):

1. Download `XNAGS40_setup.exe` (XNA Game Studio 4.0 Refresh, signed by
   Microsoft) from
   https://download.microsoft.com/download/E/C/6/EC68782D-872A-4D58-A8D3-87881995CDD4/XNAGS40_setup.exe
2. Unpack it with 7-Zip, then `redists.msi` inside it.
3. Extract the inner packages without installing them:
   `msiexec /a SharedFilesInstaller_File.msi /qn TARGETDIR=<folder>` (the
   content pipeline), and the same for `XNAFXRedist40Setup_File` (the
   framework and `XnaNative.dll`). Give each file a `.msi` extension first.
4. Put in one folder: `Microsoft.Xna.Framework.Content.Pipeline.dll` (from
   `References/Windows/x86`), the `Microsoft.Xna.Framework*.dll` of the
   framework, and `XnaNative.dll` (from `Microsoft Shared/XNA/Framework/v4.0`).
   It needs the Visual C++ 2010 runtime, 32-bit (`msvcr100.dll`).
5. Build `xnafx.exe` there with the .NET Framework 4 compiler, 32-bit:

   ```
   C:\Windows\Microsoft.NET\Framework\v4.0.30319\csc.exe -platform:x86 -out:xnafx.exe
       -r:Microsoft.Xna.Framework.Content.Pipeline.dll -r:Microsoft.Xna.Framework.dll
       -r:Microsoft.Xna.Framework.Graphics.dll <XenoForge>\tools\xna\XnaFx.cs
   ```

Then:

```
xnafx.exe tests\hlsl\*.fx -o out             # effect binaries for the Xbox 360
build\xfeffect out\*.fxb -o shaders          # their microcode, as Xenia dumps it
build\xfshader shaders\loops.0.ucode.bin.frag
set XF_SHADERS=shaders & build\shader_compare_test
```

The compiler leaves a vertex fetch's format empty (0): the console's D3D
fills it from the vertex declaration when the shader is bound. Shaders from
games, taken from traces, have it filled.
