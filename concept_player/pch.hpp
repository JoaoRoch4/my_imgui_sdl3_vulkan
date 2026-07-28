// Precompiled header for the concept_player target (see concept_player/CMakeLists.txt).
// Pulls the headers used across most concept TUs — the STL bits every file needs plus
// Dear ImGui — so they are parsed once. The vendored ImGui .cpp files opt out via
// SKIP_PRECOMPILE_HEADERS; SDL headers stay in the one TU that needs them (main.cpp).
// Mirrors launcher/pch.hpp and the app's code/pch/pch.hpp approach.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "imgui.h"
// ImHashStr() — the channel-id hash every Motion:: call keys on — is declared in the
// internal header, not imgui.h. ImAnim's own API is documented in terms of it
// (ImHashStr("height"), ImHashStr("knob")), so the concept follows suit rather than
// inventing a second hash that would key differently once ImAnim is dropped in.
#include "imgui_internal.h"
