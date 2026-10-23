#include <symbolengine/plugin/Plugin.h>

#include <symbolengine/core/Json.h>

#include "Access.h"

#include <stdexcept>
#include <string>

namespace symbolengine::plugin
{

Plugin::Plugin() = default;

Plugin::~Plugin() = default;

Buses Plugin::buses() const
{
    return {};
}

void Plugin::prepare(double, std::uint32_t) {}

void Plugin::release() {}

std::uint32_t Plugin::latency() const
{
    return 0;
}

std::uint32_t Plugin::tail() const
{
    return 0;
}

std::string Plugin::saveState() const
{
    return json::write(Object{{"parameters", saveParameters()}});
}

bool Plugin::loadState(std::string_view state)
{
    const auto saved = json::tryParse(state);
    if (!saved || !saved->isObject())
        return false;
    loadParameters((*saved)["parameters"]);
    return true;
}

void Plugin::reset() {}

std::optional<Editor> Plugin::editor() const
{
    return std::nullopt;
}

void Plugin::editorOpened(EditorLink&) {}

void Plugin::editorTick(EditorLink&) {}

void Plugin::editorClosed() {}

void Plugin::editorResized(std::uint32_t, std::uint32_t) {}

void Plugin::setEditorSize(std::uint32_t width, std::uint32_t height)
{
    editorWidth = width;
    editorHeight = height;
    if (host != nullptr)
        host->editorSizeChanged();
}

const Parameter* Plugin::findParameter(std::uint32_t id) const noexcept
{
    const auto found = indexOf.find(id);
    return found != indexOf.end() ? &definitions[found->second] : nullptr;
}

double Plugin::parameterValue(std::uint32_t id) const noexcept
{
    const auto found = indexOf.find(id);
    return found != indexOf.end() ? values[found->second].load(std::memory_order_relaxed) : 0.0;
}

ParameterValue Plugin::addParameter(Parameter parameter)
{
    const auto id = parameter.id;
    if (indexOf.contains(id))
        throw std::logic_error("parameter " + std::to_string(id) + " is added twice");
    if (!(parameter.minimum <= parameter.defaultValue && parameter.defaultValue <= parameter.maximum))
        throw std::logic_error("parameter " + std::to_string(id) + " has its default outside its range");
    if (parameter.bypass && !(parameter.stepped && parameter.minimum == 0.0 && parameter.maximum == 1.0))
        throw std::logic_error("parameter " + std::to_string(id) + " is a bypass, so it must be stepped from 0 to 1");

    indexOf.emplace(id, definitions.size());
    values.emplace_back(parameter.defaultValue);
    definitions.push_back(std::move(parameter));
    return ParameterValue(id, &values.back());
}

void Plugin::latencyChanged()
{
    if (host != nullptr)
        host->latencyChanged();
}

Object Plugin::saveParameters() const
{
    Object saved;
    for (std::size_t i = 0; i < definitions.size(); ++i)
        saved.set(std::to_string(definitions[i].id), values[i].load(std::memory_order_relaxed));
    return saved;
}

void Plugin::loadParameters(const Value& saved)
{
    // A parameter the saved state doesn't have, such as one added in a later version, takes its default, so loading
    // the same state always gives the same sound.
    for (std::size_t i = 0; i < definitions.size(); ++i)
    {
        const auto& value = saved[std::to_string(definitions[i].id)];
        detail::Access::set(*this, i, value.isNumber() ? value.asNumber() : definitions[i].defaultValue);
    }
}

}
