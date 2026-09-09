// Schema-independent read-only access, following osuplayer's RealmReader and
// osu.Game's BeatmapSet/File models. Never migrates or writes the osu database.
using Realms;
using Realms.Dynamic;
using System.Text.Json;

if (args.Length != 2) {
    Console.Error.WriteLine("usage: OsuLazerReader <osu storage directory> <output.json>");
    return 2;
}
try {
    string root = Path.GetFullPath(args[0]);
    string database = Path.Combine(root, "client.realm");
    if (!File.Exists(database)) throw new FileNotFoundException("client.realm not found", database);
    using var realm = Realm.GetInstance(new RealmConfiguration(database) {
        IsDynamic = true, IsReadOnly = true
    });
    string Asset(string hash) {
        if (hash.Length != 64 || hash.Any(c => !Uri.IsHexDigit(c)))
            throw new InvalidDataException("Invalid lazer file hash");
        return Path.Combine(root, "files", hash[..1], hash[..2], hash);
    }
    var charts = new List<object>();
    foreach (var set in realm.DynamicApi.All("BeatmapSet")) {
        if (set.DynamicApi.Get<bool>("DeletePending")) continue;
        var files = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var usage in set.DynamicApi.GetList<DynamicEmbeddedObject>("Files")) {
            string name = usage.DynamicApi.Get<string>("Filename").Replace('\\', '/');
            string hash = usage.DynamicApi.Get<IRealmObjectBase>("File").DynamicApi.Get<string>("Hash");
            files[name] = Asset(hash);
        }
        foreach (var map in set.DynamicApi.GetList<DynamicRealmObject>("Beatmaps")) {
            if (map.DynamicApi.Get<bool>("Hidden")) continue;
            var ruleset = map.DynamicApi.Get<IRealmObjectBase>("Ruleset");
            if (ruleset.DynamicApi.Get<int>("OnlineID") != 1) continue;
            var metadata = map.DynamicApi.Get<IRealmObjectBase>("Metadata");
            string audio = metadata.DynamicApi.Get<string>("AudioFile").Replace('\\', '/');
            if (!files.TryGetValue(audio, out var audioPath)) continue;
            string hash = map.DynamicApi.Get<string>("Hash");
            string source = Asset(hash);
            if (!File.Exists(source) || !File.Exists(audioPath)) continue;
            charts.Add(new { source, audio = audioPath, hash,
                set_id = set.DynamicApi.Get<Guid>("ID").ToString(),
                rating = map.DynamicApi.Get<double>("StarRating") });
        }
    }
    string temporary = args[1] + ".tmp";
    File.WriteAllText(temporary, JsonSerializer.Serialize(charts));
    File.Move(temporary, args[1], true);
    Console.Error.WriteLine($"[osu_lazer] resolved {charts.Count} native taiko charts");
    return 0;
} catch (Exception e) {
    Console.Error.WriteLine($"[osu_lazer] cannot read library: {e.Message}");
    return 1;
}
