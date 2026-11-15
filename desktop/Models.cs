using System.Reflection;
using System.Text.Json;
using System.Text.Json.Serialization;
namespace Nickel.Desktop;
public sealed class SettingDefinition
{
 public string Id{get;set;}="";public string Name{get;set;}="";public string Description{get;set;}="";
 public string Kind{get;set;}="";public double Minimum{get;set;}public double Maximum{get;set;}public double Step{get;set;}
 public double Number{get;set;}public string Text{get;set;}="";public List<string> Options{get;set;}=[];
}
public sealed class ModuleDefinition
{
 public string Id{get;set;}="";public string Name{get;set;}="";public string Description{get;set;}="";
 public string Category{get;set;}="Other";public List<SettingDefinition> Settings{get;set;}=[];
}
public sealed class Catalog
{
 public List<string> Categories{get;set;}=[];public List<ModuleDefinition> Modules{get;set;}=[];
 public static Catalog Load()
 {
  using var resource=Assembly.GetExecutingAssembly().GetManifestResourceStream("Nickel.Catalog")??throw new InvalidOperationException("Module catalogue missing.");
  using var reader=new StreamReader(resource);var script=reader.ReadToEnd();
  var json=script[(script.IndexOf('=')+1)..].Trim().TrimEnd(';');
  return JsonSerializer.Deserialize<Catalog>(json,new JsonSerializerOptions{PropertyNameCaseInsensitive=true})??throw new InvalidOperationException("Module catalogue invalid.");
 }
}
public sealed class Preferences
{
 public bool AutomaticLoad{get;set;}public bool CloseAfterLoading{get;set;}public bool StreamHide{get;set;}public int Theme{get;set;}public bool DiscordPresence{get;set;}=true;public bool UsageStatistics{get;set;}=true;public string InstallId{get;set;}="";
 public Dictionary<string,bool> Enabled{get;set;}=[];
 public Dictionary<string,Dictionary<string,JsonElement>> Values{get;set;}=[];
 public static string FilePath=>Path.Combine(AppContext.BaseDirectory,"data","preferences.json");
 public static Preferences Load(){try{return JsonSerializer.Deserialize<Preferences>(File.ReadAllText(FilePath))??new();}catch{return new();}}
 public void Save(){Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);var pending=FilePath+".tmp";File.WriteAllText(pending,JsonSerializer.Serialize(this,new JsonSerializerOptions{WriteIndented=true}));File.Move(pending,FilePath,true);}
}
public static class SelfCheck
{
 /// <summary>Counts of ui/catalog.js, written from native/generated_catalog.hpp (python tools/sync_catalog.py).</summary>
 private const int ExpectedModules=177,ExpectedSettings=915;
 public static int Run()
 {
  var report=Path.Combine(AppContext.BaseDirectory,"self-check.json");
  try{var native=NativeLoader.Payload();if(native.Length<512||native[0]!=77||native[1]!=90)throw new Exception("Native payload missing.");var catalog=Catalog.Load();if(catalog.Modules.Count!=ExpectedModules||catalog.Modules.Sum(m=>m.Settings.Count)!=ExpectedSettings)throw new Exception("Catalogue counts differ.");if(catalog.Modules.Select(m=>m.Id).Distinct().Count()!=catalog.Modules.Count)throw new Exception("Duplicate module ID.");if(!catalog.Modules.Any(m=>m.Id=="flight"&&m.Settings.Count==3))throw new Exception("Flight schema missing.");if(ReleaseTrust.SelfTest() is string trust)throw new Exception("Update checks: "+trust);if(MinecraftConnection.SelfTest() is string game)throw new Exception("Game check: "+game);File.WriteAllText(report,JsonSerializer.Serialize(new{status="passed",modules=ExpectedModules,settings=ExpectedSettings,native_payload_bytes=native.Length,mode="offline_no_game_access"}));return 0;}
  catch(Exception error){File.WriteAllText(report,JsonSerializer.Serialize(new{status="failed",message=error.Message}));return 1;}
 }
}
