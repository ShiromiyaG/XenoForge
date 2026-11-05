// xnafx: HLSL effects compiled for the Xbox 360 by XNA Game Studio 4.0's
// content pipeline, which carries Microsoft's Xbox 360 shader compiler (in
// XnaNative.dll). The effect binary it writes holds each shader as the
// console's shader container; tools/xfeffect pulls the microcode out.
//
// Builds against the XNA 4.0 assemblies (not in this repository; see
// tools/xna/README.md) with the .NET Framework 4 compiler, 32-bit:
//   csc /platform:x86 /out:xnafx.exe /r:Microsoft.Xna.Framework.Content.Pipeline.dll
//       /r:Microsoft.Xna.Framework.dll /r:Microsoft.Xna.Framework.Graphics.dll XnaFx.cs
//
// usage: xnafx <effect.fx>... [-o <folder>] [-d NAME=VALUE;...] [-windows]

using System;
using System.Collections.Generic;
using System.IO;
using Microsoft.Xna.Framework.Content.Pipeline;
using Microsoft.Xna.Framework.Content.Pipeline.Graphics;
using Microsoft.Xna.Framework.Content.Pipeline.Processors;
using Microsoft.Xna.Framework.Graphics;

class Logger : ContentBuildLogger
{
    public override void LogImportantMessage(string message, params object[] args) { Console.WriteLine(message, args); }
    public override void LogMessage(string message, params object[] args) { }
    public override void LogWarning(string helpLink, ContentIdentity identity, string message, params object[] args)
    {
        Console.Error.WriteLine("warning: " + string.Format(message, args));
    }
}

// What a processor asks of the build; only the target matters here.
class Context : ContentProcessorContext
{
    readonly TargetPlatform platform;
    readonly string output;
    readonly Logger logger = new Logger();
    readonly OpaqueDataDictionary parameters = new OpaqueDataDictionary();

    public Context(TargetPlatform platform, string output) { this.platform = platform; this.output = output; }

    public override string BuildConfiguration { get { return "Release"; } }
    public override string IntermediateDirectory { get { return output; } }
    public override ContentBuildLogger Logger { get { return logger; } }
    public override string OutputDirectory { get { return output; } }
    public override string OutputFilename { get { return Path.Combine(output, "effect.xnb"); } }
    public override OpaqueDataDictionary Parameters { get { return parameters; } }
    public override TargetPlatform TargetPlatform { get { return platform; } }
    public override GraphicsProfile TargetProfile { get { return GraphicsProfile.HiDef; } }
    public override void AddDependency(string filename) { }
    public override void AddOutputFile(string filename) { }
    public override TOutput BuildAndLoadAsset<TInput, TOutput>(ExternalReference<TInput> source, string processor,
                                                                 OpaqueDataDictionary parameters, string importer)
    {
        throw new NotSupportedException("assets are not built here");
    }
    public override ExternalReference<TOutput> BuildAsset<TInput, TOutput>(ExternalReference<TInput> source, string processor,
                                                                            OpaqueDataDictionary parameters, string importer,
                                                                            string assetName)
    {
        throw new NotSupportedException("assets are not built here");
    }
    public override TOutput Convert<TInput, TOutput>(TInput input, string processor, OpaqueDataDictionary parameters)
    {
        throw new NotSupportedException("assets are not converted here");
    }
}

static class Program
{
    static int Main(string[] args)
    {
        var inputs = new List<string>();
        string output = ".", defines = null;
        var platform = TargetPlatform.Xbox360;
        for (int i = 0; i < args.Length; i++)
        {
            if (args[i] == "-o" && i + 1 < args.Length) output = args[++i];
            else if (args[i] == "-d" && i + 1 < args.Length) defines = args[++i];
            else if (args[i] == "-windows") platform = TargetPlatform.Windows;
            else inputs.Add(args[i]);
        }
        if (inputs.Count == 0)
        {
            Console.WriteLine("usage: xnafx <effect.fx>... [-o <folder>] [-d NAME=VALUE;...] [-windows]");
            return 2;
        }
        Directory.CreateDirectory(output);
        int failed = 0;
        foreach (string input in inputs)
        {
            try
            {
                var effect = new EffectContent();
                effect.Identity = new ContentIdentity(Path.GetFullPath(input));
                effect.EffectCode = File.ReadAllText(input);
                var processor = new EffectProcessor();
                if (defines != null) processor.Defines = defines;
                CompiledEffectContent compiled = processor.Process(effect, new Context(platform, Path.GetFullPath(output)));
                byte[] code = compiled.GetEffectCode();
                string path = Path.Combine(output, Path.GetFileNameWithoutExtension(input) + ".fxb");
                File.WriteAllBytes(path, code);
                Console.WriteLine("{0}: {1} bytes", path, code.Length);
            }
            catch (Exception e)
            {
                Console.Error.WriteLine("{0}: {1}", input, e.Message);
                failed++;
            }
        }
        return failed == 0 ? 0 : 1;
    }
}
