using System.Collections;
using System.Globalization;
using System.Reflection;
using System.Windows.Input;

internal static class OriginalApp
{
    internal const BindingFlags Members = BindingFlags.Instance | BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic;
    internal static Type Type(string name) => System.Type.GetType("MovieBoxPro." + name + ", MovieBoxPro", true)!;
    internal static object? Get(object? target, string name) => target is null ? null :
        target.GetType().GetProperty(name, Members)?.GetValue(target) ?? target.GetType().GetField(name, Members)?.GetValue(target);
    internal static void Set(object? target, string name, object? value)
    {
        var property = target?.GetType().GetProperty(name, Members);
        if (property?.CanWrite != true) return;
        if (value != null && !property.PropertyType.IsInstanceOfType(value))
            value = Convert.ChangeType(value, property.PropertyType, CultureInfo.InvariantCulture);
        property.SetValue(target, value);
    }
    internal static object? Global => Type("App").GetProperty("GlobalVariable", Members)?.GetValue(null);
    internal static object? Settings => Get(Global, "SettingConstant");
    internal static string Text(object? target, string name) => Convert.ToString(Get(target, name), CultureInfo.InvariantCulture) ?? "";
    internal static double Number(object? target, string name) => Convert.ToDouble(Get(target, name) ?? 0, CultureInfo.InvariantCulture);
    internal static List<object> Items(object? value) => (value as IEnumerable)?.Cast<object>().ToList() ?? new();
    internal static object? Model(string name) => StartupHook.Models.GetValueOrDefault(name);

    // Use the receiver's actual parameter type: KeyDictionary lives in Resources.Helper,
    // and inferring it avoids silently breaking when a namespace changes again.
    internal static void Send(object target, Dictionary<string, object?> values)
    {
        var method = target.GetType().GetMethod("OnMessageReceived", Members)
            ?? throw new MissingMethodException(target.GetType().FullName, "OnMessageReceived");
        Type parameter = method.GetParameters().Single().ParameterType;
        object message = Activator.CreateInstance(parameter, target.GetType().Name, values)!;
        method.Invoke(target, new[] { message });
    }

    internal static void Command(object? target, string name, object? argument = null)
    {
        if (Get(target, name) is not ICommand command) throw new InvalidOperationException(name + " is not available yet.");
        if (!command.CanExecute(argument)) throw new InvalidOperationException(name + " is currently unavailable.");
        command.Execute(argument);
    }

    internal static async Task<string> Api(string name, params object?[] arguments)
    {
        var method = Type("Resources.Interface.MbpInterface").GetMethod(name, Members)
            ?? throw new MissingMethodException(name);
        var parameters = method.GetParameters();
        object?[] values = parameters.Select((p, i) => i < arguments.Length
            ? (arguments[i] == null || p.ParameterType.IsInstanceOfType(arguments[i]) ? arguments[i]
                : Convert.ChangeType(arguments[i], p.ParameterType, CultureInfo.InvariantCulture))
            : p.HasDefaultValue ? p.DefaultValue : throw new ArgumentException("Missing " + p.Name)).ToArray();
        return await (Task<string>)method.Invoke(null, values)!;
    }

    internal static object Analyze(string json, Type type)
    {
        var method = Type("Resources.Interface.IFHandle").GetMethods(Members)
            .Single(m => m.Name == "Analyze" && m.IsGenericMethodDefinition && m.GetParameters().Length == 3);
        object?[] args = { json, null, "" };
        return method.MakeGenericMethod(type).Invoke(null, args)
            ?? throw new InvalidOperationException("Original app API: " + Text(args[1], "msg"));
    }

    internal static void PersistSettings()
    {
        var jsonType = System.Type.GetType("Newtonsoft.Json.JsonConvert, Newtonsoft.Json", true)!;
        string json = (string)jsonType.GetMethod("SerializeObject", new[] { typeof(object) })!.Invoke(null, new[] { Settings })!;
        object record = Activator.CreateInstance(Type("Resources.Context.EntityRecordItem"))!;
        Set(record, "Property", "SettingConstant");
        Set(record, "Value", json);
        object context = Type("App").GetProperty("Context", Members)!.GetValue(null)!;
        context.GetType().GetMethod("RecordAdd")!.Invoke(context, new[] { record });
    }
}
