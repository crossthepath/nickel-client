#pragma once
// The modules of Nickel, built-in and script modules, as a component other than the menu sees them.
//
// The menu owns the module list, the preferences file (%APPDATA%\Nickel\module-preferences.ini) and the key
// bindings; components such as the Luau engine reach them only through this interface, never through the menu's
// globals. A later component (installing community configs, an assistant changing settings) uses the same
// interface. Module and Setting are the catalogue's data types (generated_catalog.hpp), shared by every component.
//
// Threads: the same rules as before the interface existed. The registry does not lock: callers keep their own
// locks (the Luau engine runs under scriptGuard).
#include <cstddef>
#include <string>

#include "generated_catalog.hpp"

namespace overlay {

class ModuleRegistry {
public:
    virtual ~ModuleRegistry() = default;

    // ---- the module list

    // Number of modules; valid indices are 0 to Count() - 1.
    virtual int Count() const = 0;

    // The module at `index`. References stay valid for the life of the registry: modules are never removed or moved.
    virtual Module& At(int index) = 0;

    // Index of the module whose id is `id`, or -1.
    virtual int Find(const std::string& id) const = 0;

    // Appends an empty module with this id and returns its index, or -1 when no more modules can be added.
    virtual int Add(const std::string& id) = 0;

    // ---- preferences

    // Records a change of module `index`: the setting `changed`, or its enabled state when `changed` is null. The
    // write to disk may be deferred.
    virtual void SaveModule(int index, const Setting* changed) = 0;

    // The saved text of `key` in section `section`, at most `maxLength` characters; empty when it is not saved.
    virtual std::string ReadPreference(const std::string& section, const std::string& key, size_t maxLength) const = 0;

    // The saved integer of `key` in section `section`, or `fallback` when it is not saved.
    virtual int ReadPreferenceInt(const std::string& section, const std::string& key, int fallback) const = 0;

    // ---- key bindings

    // The virtual-key code bound to the module `id`, or 0.
    virtual int KeyOf(const std::string& id) const = 0;

    // Binds the module `id` to key `vk`, shown as `name` in the menu.
    virtual void BindKey(const std::string& id, const std::string& name, int vk) = 0;
};

} // namespace overlay
