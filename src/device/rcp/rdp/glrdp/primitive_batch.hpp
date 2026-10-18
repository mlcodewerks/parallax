#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Each field occupies whole std140 vec4 slots. Integer coefficients retain
// their exact bits; no float conversion or vertex interpolation is involved.
class PrimitiveUniforms {
  public:
    using Slot = std::array<std::uint32_t, 4>;
    struct Field { std::string type, name; unsigned count, slot; };
    std::vector<Field> fields;
    std::vector<Slot> values;
    struct NameHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view name) const { return std::hash<std::string_view>{}(name); }
    };
    std::unordered_map<std::string, int, NameHash, std::equal_to<>> locations;
    unsigned capacity = 1;
    void initialize(const std::string &shader, unsigned max_bytes) {
        fields.clear(); values.clear(); locations.clear();
        const std::regex declaration(R"(uniform (bool|int|uint|float|[iu]?vec[234]) (\w+)(?:\[(\d+)\])?;)");
        for (std::sregex_iterator i(shader.begin(), shader.end(), declaration), end; i != end; ++i) {
            const auto &m = *i;
            add(m[1], m[2], m[3].matched ? unsigned(std::stoul(m[3])) : 1);
        }
        add("vec4", "rdp_bounds", 1);
        add("vec4", "rdp_color", 1);
        add("vec4", "rdp_uv", 1);
        capacity = std::clamp(max_bytes / unsigned(values.size() * sizeof(Slot)), 1u, 32u);
    }
    bool store(int location, const void *data, unsigned bytes) {
        if (location >= -1) return false;
        std::memcpy(values[unsigned(-location - 2)].data(), data, bytes);
        return true;
    }
    std::string block() const {
        std::string result = "struct RDPPrimitive {\n";
        for (const auto &f : fields) {
            const bool integer = f.type == "bool" || f.type == "int" || f.type[0] == 'i';
            const bool uint = f.type == "uint" || f.type[0] == 'u';
            result += std::string(integer ? "ivec4" : uint ? "uvec4" : "vec4") + " p_" + f.name;
            if (f.count > 1) result += "[" + std::to_string(f.count) + "]";
            result += ";\n";
        }
        return result + "};\nlayout(std140) uniform RDPBatch { RDPPrimitive rdp_records[" +
               std::to_string(capacity) + "]; };\n";
    }
    std::string fragment(std::string shader) const {
        std::string declarations = "flat in int rdp_instance;\n" + block();
        for (const auto &f : fields) {
            std::string declaration = "uniform " + f.type + " " + f.name;
            if (f.count > 1) declaration += "[" + std::to_string(f.count) + "]";
            declaration += ";";
            const auto at = shader.find(declaration);
            if (at == std::string::npos) continue; // A startup specialization is constant.
            shader.erase(at, declaration.size());
            std::string value = "rdp_records[rdp_instance].p_" + f.name;
            if (f.count == 1) {
                if (f.type == "bool") value = "(" + value + ".x != 0)";
                else if (f.type == "float" || f.type == "int" || f.type == "uint") value += ".x";
                else if (f.type.back() == '2') value += ".xy";
                else if (f.type.back() == '3') value += ".xyz";
            }
            declarations += "#define " + f.name + " (" + value + ")\n";
        }
        shader.insert(shader.find('\n') + 1, declarations);
        return shader;
    }
    std::string vertex() const {
        return "#version 330 core\n" + block() + R"(
flat out int rdp_instance;
out vec3 fuvq;
out vec4 shade;
void main() {
    rdp_instance = gl_InstanceID;
    vec4 b = rdp_records[gl_InstanceID].p_rdp_bounds;
    vec2 p = vec2((gl_VertexID & 1) == 0 ? b.x : b.z,
                  (gl_VertexID & 2) == 0 ? b.y : b.w);
    vec2 viewport = rdp_records[gl_InstanceID].p_viewport.xy;
    gl_Position = vec4(p.x / viewport.x * 2.0 - 1.0, 1.0 - p.y / viewport.y * 2.0, 0.0, 1.0);
    fuvq = rdp_records[gl_InstanceID].p_rdp_uv.xyz;
    shade = rdp_records[gl_InstanceID].p_rdp_color;
}
)";
    }
  private:
    void add(std::string type, std::string name, unsigned count) {
        const unsigned slot = unsigned(values.size());
        fields.push_back({std::move(type), name, count, slot});
        locations[name] = -2 - int(slot);
        for (unsigned n = 0; n < count; ++n)
            locations[name + "[" + std::to_string(n) + "]"] = -2 - int(slot + n);
        values.resize(slot + count);
    }
};

struct PrimitiveBounds {
    int x, y, w, h;
    bool intersects(const PrimitiveBounds &b) const {
        return x < b.x + b.w && b.x < x + w && y < b.y + b.h && b.y < y + h;
    }
};
