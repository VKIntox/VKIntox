#!/usr/bin/env bash
#
# VKIntox installer for Sober (Flatpak Roblox launcher).
#
# Builds the layer inside the Flatpak SDK, drops the ReShade shader packages
# into the right config directory, and dynamically builds shader_manager.conf
# so the preprocessor resolves all relative subfolder includes natively.
#
set -euo pipefail

# --- output helpers -----------------------------------------------------------
if [[ -t 1 ]]; then
    B='\033[1m'; BLUE='\033[34m'; GREEN='\033[32m'; YELLOW='\033[33m'; RED='\033[31m'; DIM='\033[2m'; NC='\033[0m'
else
    B=''; BLUE=''; GREEN=''; YELLOW=''; RED=''; DIM=''; NC=''
fi
say()  { printf "${BLUE}  →${NC} %s\n" "$*"; }
ok()   { printf "${GREEN}  ✓${NC} %s\n" "$*"; }
warn() { printf "${YELLOW}  !${NC} %s\n" "$*"; }
die()  { printf "${RED}  ✗${NC} %s\n" "$*"; exit 1; }
step() { printf "\n${BLUE}${B}[%s]${NC}\n" "$*"; }

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "$1 is required (install it via your package manager)."
}

# --- 1. dependencies ----------------------------------------------------------
step "Checking dependencies"
need_cmd flatpak
need_cmd just
need_cmd curl
need_cmd unzip
need_cmd python3
ok "All required tools are present"

if flatpak remote-list --user 2>/dev/null | grep -q '^flathub'; then
    ok "Flathub remote is configured"
else
    warn "Flathub remote missing — adding it now"
    flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
fi

# --- 2. SDKs ------------------------------------------------------------------
step "Installing Flatpak SDKs"
say "Answer 'y' if flatpak prompts about remotes or permissions."

flatpak install -y --user flathub runtime/org.gnome.Sdk/x86_64/50 || die "GNOME SDK install failed"
flatpak install -y --user flathub runtime/org.freedesktop.Sdk/x86_64/25.08 || die "Freedesktop SDK install failed"

# --- 3. build the layer -------------------------------------------------------
step "Building VKIntox (just flatpak-build)"
just flatpak-build || die "Build failed — see output above."

# --- 4. directory setup -------------------------------------------------------
step "Setting up configuration directory"

SOBER_CONFIG="$HOME/.var/app/org.vinegarhq.Sober/config/VKIntox"
RESHADE_DIR="$SOBER_CONFIG/reshade"
SHADERS_DIR="$RESHADE_DIR/Shaders"
TEXTURES_DIR="$RESHADE_DIR/Textures"
SHADER_MANAGER_CONF="$SOBER_CONFIG/shader_manager.conf"
FONT_DIR="$SOBER_CONFIG/font"

# Clean up legacy include folder if present to avoid pollution
rm -rf "$SHADERS_DIR/include" "$SHADERS_DIR/Include"
mkdir -p "$SHADERS_DIR" "$TEXTURES_DIR" "$FONT_DIR"

# Copy the bundled font if present (Flatpak assets).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -f "$SCRIPT_DIR/assets/font/font.ttf" ]]; then
    cp "$SCRIPT_DIR/assets/font/font.ttf" "$FONT_DIR/"
    ok "Installed font.ttf"
fi
[[ -f "$SCRIPT_DIR/assets/font/LICENSE" ]] && cp "$SCRIPT_DIR/assets/font/LICENSE" "$FONT_DIR/"

# --- 5. fetch + extract official shader packages ------------------------------
step "Installing ReShade shader packages"
WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

INI_FILE="$PWD/EffectPackages.ini"

python3 - "$WORK_DIR" "$SHADERS_DIR" "$TEXTURES_DIR" "$INI_FILE" <<'PYEOF'
import configparser, os, sys, shutil, urllib.request, zipfile

work_dir, shaders_dir, textures_dir, ini_file = sys.argv[1:5]

with open(ini_file, "r", encoding="utf-8-sig") as f:
    ini_text = f.read()

cfg = configparser.ConfigParser(strict=False)
cfg.read_string(ini_text)

ok = 0
failed = []

for section in cfg.sections():
    pkg = cfg[section]
    name = pkg.get("PackageName", section)
    url = pkg.get("DownloadUrl")
    if not url:
        continue

    raw = pkg.get("InstallPath", r".\reshade-shaders\Shaders").replace("\\", "/")
    raw = raw.lstrip("./")
    sub_shader = raw.replace("reshade-shaders/Shaders", "", 1).strip("/")
    sub_texture = pkg.get("TextureInstallPath", r".\reshade-shaders\Textures") \
        .replace("\\", "/").lstrip("./").replace("reshade-shaders/Textures", "", 1).strip("/")

    target_shaders = os.path.join(shaders_dir, sub_shader) if sub_shader else shaders_dir
    target_textures = os.path.join(textures_dir, sub_texture) if sub_texture else textures_dir
    os.makedirs(target_shaders, exist_ok=True)
    os.makedirs(target_textures, exist_ok=True)

    effect_files = [f.strip() for f in pkg.get("EffectFiles", "").split(",") if f.strip()]
    deny_files = set(f.strip() for f in pkg.get("DenyEffectFiles", "").split(",") if f.strip())

    zip_path = os.path.join(work_dir, f"{section}.zip")
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(req) as r, open(zip_path, "wb") as f:
            shutil.copyfileobj(r, f)
        with zipfile.ZipFile(zip_path) as z:
            z.extractall(os.path.join(work_dir, section))
    except Exception as e:
        failed.append(f"{name} (download/extract: {e})")
        continue

    n_shaders = n_textures = 0
    for root, _dirs, files in os.walk(os.path.join(work_dir, section)):
        for fn in files:
            src = os.path.join(root, fn)
            if fn in deny_files:
                continue
            if fn.endswith(".fx") or fn.endswith(".fxh"):
                if fn.endswith(".fx") and effect_files and fn not in effect_files:
                    continue
                shutil.copy2(src, target_shaders)
                n_shaders += 1
            elif fn.lower().endswith((".png", ".jpg", ".jpeg", ".dds", ".tga", ".bmp")):
                shutil.copy2(src, target_textures)
                n_textures += 1
    if n_shaders == 0 and n_textures == 0:
        failed.append(f"{name} (no matching files)")
    else:
        ok += 1
        print(f"  ✓ Downloaded {name}: {n_shaders} shader(s), {n_textures} texture(s)")

print()
print(f"  {ok} package(s) installed")
if failed:
    print(f"  {len(failed)} package(s) had problems:")
    for f in failed:
        print(f"    - {f}")
PYEOF

# --- 6. generate shader_manager.conf -----------------------------------------
step "Configuring search paths in shader_manager.conf"

# Build search path list: base Shaders directory plus all subdirectories
ALL_PATHS=("$SHADERS_DIR")
while IFS= read -r -d '' dir; do
    ALL_PATHS+=("$dir")
done < <(find "$SHADERS_DIR" -mindepth 1 -type d -print0)

# Join paths with commas
JOINED_PATHS=$(IFS=,; echo "${ALL_PATHS[*]}")

# Build texture path list
ALL_TEX_PATHS=("$TEXTURES_DIR")
while IFS= read -r -d '' dir; do
    ALL_TEX_PATHS+=("$dir")
done < <(find "$TEXTURES_DIR" -mindepth 1 -type d -print0)
JOINED_TEX_PATHS=$(IFS=,; echo "${ALL_TEX_PATHS[*]}")

{
    echo "parentDir = $RESHADE_DIR/"
    echo "shaderPath = $JOINED_PATHS"
    echo "texturePath = $JOINED_TEX_PATHS"
} > "$SHADER_MANAGER_CONF"

ok "Wrote dynamic paths to $SHADER_MANAGER_CONF"

# --- 7. enable the layer in Sober --------------------------------------------
step "Enabling VKIntox in Sober"
if flatpak override --user org.vinegarhq.Sober --env=ENABLE_VKINTOX=1 2>/dev/null; then
    ok "ENABLE_VKINTOX=1 set"
else
    warn "Couldn't set the override — Sober may not be installed yet."
    warn "After installing Sober, run:"
    printf "    ${DIM}flatpak override --user org.vinegarhq.Sober --env=ENABLE_VKINTOX=1${NC}\n"
fi

# --- 8. summary ---------------------------------------------------------------
SHADER_COUNT=$(find "$SHADERS_DIR" -name '*.fx' 2>/dev/null | wc -l)
TEXTURE_COUNT=$(find "$TEXTURES_DIR" -type f 2>/dev/null | wc -l)

step "Done"
printf "  shaders:    ${DIM}%s${NC}  (%s)\n" "$SHADER_COUNT" "$SHADERS_DIR"
printf "  textures:   ${DIM}%s${NC}  (%s)\n" "$TEXTURE_COUNT" "$TEXTURES_DIR"
printf "  config:     ${DIM}%s${NC}\n" "$SHADER_MANAGER_CONF"
printf "\n  ${B}Launch Sober to start using VKIntox.${NC}\n"
