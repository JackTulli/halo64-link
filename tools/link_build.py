"""Ninja rules for Halo 64 Link (``ninja link-x86``; link/README.md), the
computer side of the Nintendo 64 port's online play.

The Link is the game itself, as a CLIENT, built as the dedicated server is
(tools/server_build.py): the same sources and network code as the 32-bit
Linux build (tools/linux_build.py), without a window, input or sound. SDL's
window and input are replaced by the Link's own platform units
(link/platform: link_platform.c, link_input.c), SDL's few utility functions
by the server's (server/platform/sdl_headless.c), so the program links only
the C library. Like the server, it needs SDL's headers to compile, fetched
once by tools/server_build.py, never its library. None of the server's
control API, web admin page or Delta Control units are in it: a Link is a
player's machine, not a server.

HALO_LINK is defined for every unit (HALO_SERVER is not: the Link joins and
hosts games as a player's copy does; link/README.md lists what each
HALO_SERVER use does in the Link).

Targets, where this machine can link them:
  link-x86      32-bit x86, the build that plays the Xbox maps: an x86 host,
                or an x86-64 glibc host with 32-bit glibc
  link          the same
Each writes build/link-x86/halo64-link.
"""

from pathlib import Path
from types import SimpleNamespace
from typing import Any, List

from .embed_assets import hud_assets_build, ui_fonts_build
from .linux_build import (
    PORT_CONFIG,
    QRCODEGEN_DIR,
    MBEDTLS_DIR,
    MONOCYPHER_DIR,
    Linux32Units,
    _load_port_config,
    compile_launcher,
    linux32_objects,
    lto_flags,
)
from .ninja_syntax import Writer
from .server_build import (
    SDL_INCLUDE,
    SERVER_ARCHES,
    SERVER_PLATFORM_DIR,
    _link_flags,
    _link_rule,
    buildable_arches,
    fetch_sdl_headers,
    host_libc,
)

LINK_DIR = Path("link")
LINK_PLATFORM_DIR = LINK_DIR / "platform"
# the Link's own platform units, with the Xbox's ABI (in place of
# sdl_platform.c and xinput_sdl.c)
LINK_PLATFORM_SOURCES = [LINK_PLATFORM_DIR / "link_platform.c", LINK_PLATFORM_DIR / "link_input.c"]
# and with the host's ABI: SDL's few utility functions and glibc's
# backtrace, the server's
LINK_NATIVE_SOURCES = [SERVER_PLATFORM_DIR / "sdl_headless.c", SERVER_PLATFORM_DIR / "backtrace.c"]
# the window, input and self-updater the Link has none of
LINK_EXCLUDED = {
    "port/linux/src/sdl_platform.c",
    "port/linux/src/xinput_sdl.c",
    "port/linux/src/updater.c",
}
LINK_DEFINES = ["-DHALO_LINK"]
OUTPUT_NAME = "halo64-link"
# only the 32-bit build: it plays the Xbox maps the cartridge's are made from
LINK_ARCHES = ["x86"]


def link_configure_inputs() -> List[Path]:
    """Files whose change must re-run configure.py."""
    return [Path(__file__), LINK_PLATFORM_DIR]


def generate_link_build(n: Writer, sln: Any) -> None:
    if not PORT_CONFIG.is_file():
        return
    arches = [name for name in buildable_arches() if name in LINK_ARCHES]
    if not arches or not fetch_sdl_headers():
        return
    config = _load_port_config()
    static = host_libc() == "musl"
    defines = [*LINK_DEFINES, *(["-DHALO_MUSL"] if static else [])]
    cc = sln.linux_cc or "clang"
    # (the game list and invites: the Link lists and joins internet games)
    link_sln = SimpleNamespace(**vars(sln))
    link_sln.game_browser = True
    libs = "-lm -lpthread"
    include_flags = [f"-I{SDL_INCLUDE}", f"-I{MONOCYPHER_DIR}", f"-I{MBEDTLS_DIR / 'include'}", f"-I{QRCODEGEN_DIR}"]

    n.comment(f"Halo 64 Link (ninja link-x86; tools/link_build.py): {', '.join(arches)}, "
              f"{'static, musl' if static else 'glibc'}")
    outputs = []
    for name in arches:
        arch = SERVER_ARCHES[name]
        rule_name = f"link-{name}"
        build_dir: Path = sln.build_dir / f"link-{name}"
        output = build_dir / OUTPUT_NAME
        target = arch.musl_target if static else arch.gnu_target
        lto_cflags, lto_ldflags = lto_flags(sln, build_dir / "thinlto-cache")
        generated = (hud_assets_build(n, rule_name, build_dir / "generated" / "hud_hires_assets.c")
                     + ui_fonts_build(n, rule_name, build_dir / "generated" / "ui_fonts.c", link_sln))
        n.variable(f"{rule_name}_cc", cc)
        n.rule(
            name=f"{rule_name}_cc",
            command=f"{compile_launcher(sln)}${rule_name}_cc -MMD -MF $out.d $cflags -c $in -o $out",
            description=f"{rule_name.upper()} CC $out",
            depfile="$out.d",
            deps="gcc",
        )
        # (the 32-bit game's MSVC semantics headers, ``ninja linux``'s)
        linux_dir = sln.build_dir / "linux"
        units = Linux32Units(
            sln=link_sln, config=config, game_browser=True,
            semantics_header=linux_dir / "halo_msvc_semantics.h",
            platform_semantics_header=linux_dir / "platform_msvc_semantics.h",
            embedded_assets=generated, rule=f"{rule_name}_cc", target_flags=target,
            extra_flags=[*arch.flags, *defines], excluded=LINK_EXCLUDED,
            platform_sources=LINK_PLATFORM_SOURCES, native_sources=LINK_NATIVE_SOURCES,
            include_flags=include_flags,
        )
        objects = linux32_objects(n, units, build_dir / "obj", lto_cflags, [])
        _link_rule(n, rule_name, static)
        n.build(
            outputs=output,
            rule=f"{rule_name}_link",
            inputs=objects,
            variables={"ldflags": _link_flags(static, target, lto_ldflags), "libs": libs},
            implicit=[Path("tools/linux_link_check.py")],
        )
        n.build(outputs=f"link-{name}", rule="phony", inputs=output)
        outputs.append(output)
    n.build(outputs="link", rule="phony", inputs=outputs)
    n.newline()
