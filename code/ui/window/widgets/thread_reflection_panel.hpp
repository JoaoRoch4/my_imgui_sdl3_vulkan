#pragma once
#include "pch.hpp"

// Live, read-only ImGui table of every ManagedThread (from ThreadRegistry::snapshot()).
// Toggled from the app debug menu; renders nothing when `open` points to false.
class ThreadReflectionPanel {
public:
    void draw(bool *open);
};
