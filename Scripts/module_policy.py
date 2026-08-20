#!/usr/bin/env python3
"""Strict manifest-v2 and CMake module-target inventory helpers.

Architecture gates intentionally consume CMake's generated target inventory
instead of guessing ownership from source paths, CMake text, or RVX target
spelling.  This module centralises the fail-closed parsing rules so every gate
observes the same module graph.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

from architecture_gate_common import GateInputError, resolve_required_file


MANIFEST_SCHEMA = "RVX.ModuleBoundaryManifest"
INVENTORY_SCHEMA = "RVX.ModuleTargetInventory"
MANIFEST_FIELDS = {
    "id", "path", "domain", "layer", "publicIncludePrefixes",
    "allowedModules", "shippingAllowed", "deferred", "owner",
}
INVENTORY_FIELDS = {
    "name", "aliases", "module", "type", "sourceDirectory",
    "binaryDirectory", "infrastructure", "sources", "linkLibraries",
    "interfaceLinkLibraries", "includeDirectories",
    "interfaceIncludeDirectories",
}
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".mm"}
PROJECT_REFERENCE = re.compile(
    r"\b(?:RVX(?:::|_)?[A-Za-z0-9_]+|RenderVerse[A-Za-z0-9_]+|"
    r"Particle[A-Za-z0-9_]*)\b"
)
GENEX_WRAPPER = re.compile(r"^\$<[A-Za-z0-9_]+:(.*)>$")
DYNAMIC_TARGET_GENEX = re.compile(
    r"\$<(?:TARGET_PROPERTY|GENEX_EVAL|TARGET_GENEX_EVAL):"
)
SIMPLE_INCLUDE_GENEX = re.compile(
    r"^\$<(?:BUILD_INTERFACE|INSTALL_INTERFACE):[^$<>]*>$"
)
INCLUDE_PREFIX = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
VALID_DOMAINS = frozenset({"runtime", "developer", "editor", "samples", "tests"})
VALID_LAYERS = frozenset({
    "adapter", "backend", "composition", "contract", "feature", "foundation",
    "platform", "product", "render", "tooling", "validation",
})


@dataclass(frozen=True)
class Module:
    id: str
    path: Path
    domain: str
    layer: str
    public_include_prefixes: tuple[str, ...]
    allowed_modules: frozenset[str]
    shipping_allowed: bool
    deferred: bool
    owner: str


@dataclass(frozen=True)
class Target:
    name: str
    aliases: tuple[str, ...]
    module: str
    type: str
    source_directory: str
    binary_directory: str
    infrastructure: bool
    sources: tuple[str, ...]
    link_libraries: tuple[str, ...]
    interface_link_libraries: tuple[str, ...]
    include_directories: tuple[str, ...]
    interface_include_directories: tuple[str, ...]


@dataclass(frozen=True)
class Policy:
    root: Path
    manifest_path: Path
    inventory_path: Path
    modules: dict[str, Module]
    prefix_to_module: dict[str, str]
    targets: tuple[Target, ...]
    target_to_module: dict[str, str]
    disabled_modules: frozenset[str]
    source_root: Path
    binary_root: Path

    def active_module_ids(self) -> frozenset[str]:
        return frozenset(
            target.module for target in self.targets
            if target.module and not target.infrastructure and target.module not in self.disabled_modules
        )

    def module_for_prefix(self, prefix: str) -> str | None:
        return self.prefix_to_module.get(prefix.casefold())

    def module_for_path(self, path: Path) -> str | None:
        try:
            relative = path.resolve().relative_to(self.root)
        except ValueError:
            return None
        matches = [
            module for module in self.modules.values()
            if relative == module.path or relative.parts[:len(module.path.parts)] == module.path.parts
        ]
        if len(matches) == 1:
            return matches[0].id
        return None

    def source_files(self, module_ids: Iterable[str] | None = None) -> list[Path]:
        """Return all source-like files under Manifest-owned module paths.

        Target ``SOURCES`` is intentionally not used here: CMake commonly
        omits public headers, while source-policy gates must inspect them too.
        Concrete target source ownership is checked separately.
        """
        requested = set(module_ids) if module_ids is not None else set(self.modules)
        files: set[Path] = set()
        for module_id in requested:
            module = self.modules[module_id]
            module_root = (self.root / module.path).resolve()
            if not module_root.is_dir():
                continue
            for path in module_root.rglob("*"):
                if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES:
                    files.add(path.resolve())
        return sorted(files)

    def target_source_files(self, module_ids: Iterable[str]) -> list[Path]:
        """Return concrete, non-infrastructure target sources from inventory."""
        requested = set(module_ids)
        files: set[Path] = set()
        for target in self.targets:
            if target.infrastructure or target.module not in requested:
                continue
            for value in target.sources:
                path = resolve_target_source(self, target, value)
                if path is not None and path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES:
                    files.add(path.resolve())
        return sorted(files)

    def public_header_files(self, module_ids: Iterable[str] | None = None) -> list[tuple[str, Path]]:
        requested = set(module_ids) if module_ids is not None else set(self.modules)
        result: list[tuple[str, Path]] = []
        for module_id in sorted(requested):
            module = self.modules[module_id]
            include_root = self.root / module.path / "Include"
            if not include_root.is_dir():
                continue
            for path in sorted(include_root.rglob("*")):
                if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES:
                    result.append((module_id, path))
        return result


def add_policy_arguments(parser: Any, *, inventory_required: bool = True) -> None:
    parser.add_argument("--manifest", default=None, help="Manifest v2 JSON path.")
    parser.add_argument("--config", default=None, help="Deprecated alias for --manifest.")
    parser.add_argument(
        "--module-inventory",
        required=inventory_required,
        help="Generated RVX module target inventory JSON path.",
    )


def manifest_argument(args: Any) -> str:
    manifest = getattr(args, "manifest", None)
    config = getattr(args, "config", None)
    if manifest and config and Path(manifest) != Path(config):
        raise GateInputError("--manifest and deprecated --config disagree")
    return manifest or config or "Docs/module-boundaries.json"


def read_json(root: Path, raw_path: str, label: str) -> tuple[dict[str, Any], Path]:
    path = resolve_required_file(root, raw_path, label)
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise GateInputError(f"{label} is invalid JSON: {error}") from error
    if not isinstance(data, dict):
        raise GateInputError(f"{label} root must be an object")
    return data, path


def _nonempty_string(value: Any, context: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise GateInputError(f"{context} must be a non-empty string")
    return value.strip()


def _relative_path(value: Any, context: str) -> Path:
    raw = _nonempty_string(value, context).replace("\\", "/")
    path = Path(raw)
    if path.is_absolute() or ".." in path.parts or "." in path.parts:
        raise GateInputError(f"{context} must be a safe relative path")
    return path


def _string_array(value: Any, context: str, *, allow_empty: bool = False) -> list[str]:
    if not isinstance(value, list) or (not value and not allow_empty):
        raise GateInputError(f"{context} must be a {'possibly empty ' if allow_empty else 'non-empty '}array")
    result = [_nonempty_string(item, f"{context} entry") for item in value]
    if len({item.casefold() for item in result}) != len(result):
        raise GateInputError(f"{context} contains duplicate entries ignoring case")
    return result


def load_manifest(root: Path, raw_path: str) -> tuple[dict[str, Module], dict[str, str], Path]:
    data, path = read_json(root, raw_path, "module manifest")
    if data.get("schema") != MANIFEST_SCHEMA or data.get("schemaVersion") != 2:
        raise GateInputError(f"module manifest must use {MANIFEST_SCHEMA} schemaVersion 2")
    entries = data.get("modules")
    if not isinstance(entries, list) or not entries:
        raise GateInputError("module manifest modules must be a non-empty array")

    modules: dict[str, Module] = {}
    module_ids_casefold: set[str] = set()
    paths_casefold: set[str] = set()
    module_paths: list[Path] = []
    prefixes: dict[str, str] = {}
    allowed_values: dict[str, list[str]] = {}
    for index, entry in enumerate(entries):
        context = f"module manifest modules[{index}]"
        if not isinstance(entry, dict):
            raise GateInputError(f"{context} must be an object")
        keys = set(entry)
        if keys != MANIFEST_FIELDS:
            missing = sorted(MANIFEST_FIELDS - keys)
            extra = sorted(keys - MANIFEST_FIELDS)
            detail = []
            if missing:
                detail.append("missing " + ", ".join(missing))
            if extra:
                detail.append("unsupported " + ", ".join(extra))
            raise GateInputError(f"{context} fields are invalid: {'; '.join(detail)}")
        module_id = _nonempty_string(entry["id"], f"{context}.id")
        if "*" in module_id or module_id.casefold() in module_ids_casefold:
            raise GateInputError(f"{context}.id must be unique and may not contain '*'")
        module_ids_casefold.add(module_id.casefold())
        module_path = _relative_path(entry["path"], f"{context}.path")
        if module_path.as_posix().casefold() in paths_casefold:
            raise GateInputError(f"{context}.path must be unique ignoring case")
        folded_parts = tuple(part.casefold() for part in module_path.parts)
        for existing in module_paths:
            existing_parts = tuple(part.casefold() for part in existing.parts)
            if (folded_parts[:len(existing_parts)] == existing_parts or
                    existing_parts[:len(folded_parts)] == folded_parts):
                raise GateInputError(
                    f"{context}.path overlaps module path {existing.as_posix()}"
                )
        paths_casefold.add(module_path.as_posix().casefold())
        module_paths.append(module_path)
        if not (root / module_path).is_dir():
            raise GateInputError(f"{context}.path does not exist: {module_path.as_posix()}")
        domain = _nonempty_string(entry["domain"], f"{context}.domain")
        layer = _nonempty_string(entry["layer"], f"{context}.layer")
        if domain not in VALID_DOMAINS:
            raise GateInputError(f"{context}.domain is not a supported domain: {domain}")
        if layer not in VALID_LAYERS:
            raise GateInputError(f"{context}.layer is not a supported layer: {layer}")
        owner = _nonempty_string(entry["owner"], f"{context}.owner")
        include_prefixes = _string_array(entry["publicIncludePrefixes"], f"{context}.publicIncludePrefixes")
        allowed = _string_array(entry["allowedModules"], f"{context}.allowedModules")
        if any("*" in item for item in (*include_prefixes, *allowed)):
            raise GateInputError(f"{context} forbids wildcard prefixes and allowed modules")
        if any(INCLUDE_PREFIX.fullmatch(prefix) is None for prefix in include_prefixes):
            raise GateInputError(f"{context}.publicIncludePrefixes must contain single include tokens")
        if not isinstance(entry["shippingAllowed"], bool) or not isinstance(entry["deferred"], bool):
            raise GateInputError(f"{context}.shippingAllowed and .deferred must be booleans")
        for prefix in include_prefixes:
            key = prefix.casefold()
            if key in prefixes:
                raise GateInputError(f"public include prefix '{prefix}' is not unique ignoring case")
            prefixes[key] = module_id
        modules[module_id] = Module(
            id=module_id, path=module_path, domain=domain, layer=layer,
            public_include_prefixes=tuple(include_prefixes),
            allowed_modules=frozenset(allowed), shipping_allowed=entry["shippingAllowed"],
            deferred=entry["deferred"], owner=owner,
        )
        allowed_values[module_id] = allowed

    for module_id, allowed in allowed_values.items():
        unknown = sorted(item for item in allowed if item not in modules)
        if unknown:
            raise GateInputError(f"module {module_id} allows unknown modules: {', '.join(unknown)}")
    backend_prefixes = {
        "RHI_DX11": "DX11", "RHI_DX12": "DX12", "RHI_Vulkan": "Vulkan",
        "RHI_Metal": "Metal", "RHI_OpenGL": "OpenGL",
    }
    for module_id, prefix in backend_prefixes.items():
        module = modules.get(module_id)
        if module is None or prefix not in module.public_include_prefixes:
            raise GateInputError(f"module {module_id} must declare backend public include prefix {prefix}")
    editor = modules.get("Editor")
    if editor is None or not editor.deferred:
        raise GateInputError("Editor must exist in the manifest and be marked deferred")
    return modules, prefixes, path


def _string_list(entry: dict[str, Any], field: str, context: str) -> tuple[str, ...]:
    value = entry[field]
    if not isinstance(value, list) or any(not isinstance(item, str) for item in value):
        raise GateInputError(f"{context}.{field} must be an array of strings")
    return tuple(item for item in value if item.strip())


def load_policy(root: Path, args: Any) -> Policy:
    modules, prefixes, manifest_path = load_manifest(root, manifest_argument(args))
    data, inventory_path = read_json(root, args.module_inventory, "module target inventory")
    if data.get("schema") != INVENTORY_SCHEMA or data.get("schemaVersion") != 1:
        raise GateInputError(f"module target inventory must use {INVENTORY_SCHEMA} schemaVersion 1")
    for key in ("configuration", "sourceRoot", "binaryRoot", "targets", "disabledModules"):
        if key not in data:
            raise GateInputError(f"module target inventory is missing '{key}'")
    _nonempty_string(data["configuration"], "module target inventory.configuration")
    if not isinstance(data["targets"], list) or not data["targets"]:
        raise GateInputError("module target inventory targets must be a non-empty array")
    source_root = Path(_nonempty_string(data["sourceRoot"], "module target inventory.sourceRoot")).resolve()
    binary_root = Path(_nonempty_string(data["binaryRoot"], "module target inventory.binaryRoot")).resolve()
    if source_root != root.resolve():
        raise GateInputError(f"module target inventory sourceRoot does not match --root: {source_root}")
    if not binary_root.is_dir():
        raise GateInputError(f"module target inventory binaryRoot does not exist: {binary_root}")
    if source_root == binary_root or source_root.is_relative_to(binary_root):
        raise GateInputError("module target inventory binaryRoot must not contain sourceRoot")

    disabled_raw = data["disabledModules"]
    if not isinstance(disabled_raw, list):
        raise GateInputError("module target inventory disabledModules must be an array")
    disabled: set[str] = set()
    for index, entry in enumerate(disabled_raw):
        if not isinstance(entry, dict) or set(entry) != {"module", "reason"}:
            raise GateInputError(f"disabledModules[{index}] must contain exactly module and reason")
        module_id = _nonempty_string(entry["module"], f"disabledModules[{index}].module")
        _nonempty_string(entry["reason"], f"disabledModules[{index}].reason")
        if module_id not in modules or module_id in disabled:
            raise GateInputError(f"disabledModules[{index}] names an unknown or duplicate module")
        disabled.add(module_id)
    missing_deferred = sorted(
        module_id for module_id, module in modules.items()
        if module.deferred and module_id not in disabled
    )
    if missing_deferred:
        raise GateInputError(
            "deferred modules must be recorded in inventory.disabledModules: "
            + ", ".join(missing_deferred)
        )

    targets: list[Target] = []
    target_to_module: dict[str, str] = {}
    for index, entry in enumerate(data["targets"]):
        context = f"module target inventory.targets[{index}]"
        if not isinstance(entry, dict) or set(entry) != INVENTORY_FIELDS:
            missing = INVENTORY_FIELDS - set(entry) if isinstance(entry, dict) else INVENTORY_FIELDS
            extra = set(entry) - INVENTORY_FIELDS if isinstance(entry, dict) else set()
            raise GateInputError(f"{context} fields are invalid: missing={sorted(missing)} extra={sorted(extra)}")
        name = _nonempty_string(entry["name"], f"{context}.name")
        aliases = _string_list(entry, "aliases", context)
        module_id = _nonempty_string(entry["module"], f"{context}.module")
        infrastructure = entry["infrastructure"]
        if not isinstance(infrastructure, bool):
            raise GateInputError(f"{context}.infrastructure must be boolean")
        if not infrastructure and module_id not in modules:
            raise GateInputError(f"{context}.module is not a manifest module: {module_id}")
        if infrastructure and module_id and module_id not in modules:
            raise GateInputError(f"{context}.module must be empty or a manifest module")
        names = (name, *aliases)
        for target_name in names:
            if target_name in target_to_module:
                raise GateInputError(f"duplicate target or alias in module target inventory: {target_name}")
            target_to_module[target_name] = module_id
        target_type = _nonempty_string(entry["type"], f"{context}.type")
        if infrastructure and target_type not in {"INTERFACE_LIBRARY", "UTILITY"}:
            raise GateInputError(
                f"{context}.infrastructure requires INTERFACE_LIBRARY or UTILITY type"
            )
        sources = _string_list(entry, "sources", context)
        link_libraries = _string_list(entry, "linkLibraries", context)
        interface_link_libraries = _string_list(
            entry, "interfaceLinkLibraries", context
        )
        include_directories = _string_list(entry, "includeDirectories", context)
        interface_include_directories = _string_list(
            entry, "interfaceIncludeDirectories", context
        )
        for field, values in (
            ("linkLibraries", link_libraries),
            ("interfaceLinkLibraries", interface_link_libraries),
            ("includeDirectories", include_directories),
            ("interfaceIncludeDirectories", interface_include_directories),
        ):
            for value in values:
                normalized = value.replace("__RVX_GENEX_OPEN__", "$<")
                if DYNAMIC_TARGET_GENEX.search(normalized):
                    raise GateInputError(
                        f"{context}.{field} contains an unauditable dynamic "
                        f"target-property generator expression: {value}"
                    )
                if (field in {"includeDirectories", "interfaceIncludeDirectories"}
                        and "$<" in normalized
                        and SIMPLE_INCLUDE_GENEX.fullmatch(normalized) is None):
                    raise GateInputError(
                        f"{context}.{field} contains an unsupported include-directory "
                        f"generator expression: {value}"
                    )

        targets.append(Target(
            name=name, aliases=aliases, module=module_id,
            type=target_type,
            source_directory=_nonempty_string(entry["sourceDirectory"], f"{context}.sourceDirectory"),
            binary_directory=_nonempty_string(entry["binaryDirectory"], f"{context}.binaryDirectory"),
            infrastructure=infrastructure,
            sources=sources,
            link_libraries=link_libraries,
            interface_link_libraries=interface_link_libraries,
            include_directories=include_directories,
            interface_include_directories=interface_include_directories,
        ))
    if not targets:
        raise GateInputError("module target inventory has no targets")
    active_modules = {target.module for target in targets if not target.infrastructure}
    for module_id in active_modules:
        if module_id in disabled:
            raise GateInputError(f"module {module_id} is both active and disabled")
        if modules[module_id].deferred:
            raise GateInputError(f"deferred module {module_id} has an active target")
    for module_id, module in modules.items():
        if module.deferred:
            continue
        if (module_id in active_modules) == (module_id in disabled):
            state = "both active and disabled" if module_id in active_modules else "neither active nor disabled"
            raise GateInputError(f"non-deferred module {module_id} is {state}")
    return Policy(root=root.resolve(), manifest_path=manifest_path, inventory_path=inventory_path,
                  modules=modules, prefix_to_module=prefixes, targets=tuple(targets),
                  target_to_module=target_to_module, disabled_modules=frozenset(disabled),
                  source_root=source_root, binary_root=binary_root)


def unwrap_cmake_value(value: str) -> list[str]:
    """Return possible target/path atoms after CMake genex/CMP0079 wrappers."""
    text = value.strip().strip('"').replace("__RVX_GENEX_OPEN__", "$<")
    # CMP0079 directory lookup wrappers: ::@(directory-id);target;::@
    text = re.sub(r"::@\([^)]*\);?", "", text)
    text = text.replace("::@", "")
    previous = None
    while text != previous:
        previous = text
        match = GENEX_WRAPPER.fullmatch(text)
        if match:
            text = match.group(1).strip()
    return [
        part.strip() for part in text.split(";")
        if part.strip() and not part.strip().startswith("$<")
    ]


def resolve_target_reference(policy: Policy, value: str) -> tuple[str | None, list[str]]:
    resolved: str | None = None
    unknown: list[str] = []
    normalized = value.strip().strip('"').replace("__RVX_GENEX_OPEN__", "$<")
    atoms = [*unwrap_cmake_value(value), *PROJECT_REFERENCE.findall(normalized)]
    for atom in dict.fromkeys(atoms):
        module_id = policy.target_to_module.get(atom)
        if module_id is not None:
            if resolved is not None and resolved != module_id:
                unknown.append(atom)
            else:
                resolved = module_id
            continue
        candidates = PROJECT_REFERENCE.findall(atom)
        if candidates:
            unknown.extend(candidates)
    return resolved, unknown


def resolve_public_target_reference(policy: Policy, value: str) -> tuple[str | None, list[str]]:
    """Resolve an interface edge, excluding CMake's private LINK_ONLY export."""
    text = value.strip().strip('"').replace("__RVX_GENEX_OPEN__", "$<")
    text = re.sub(r"::@\([^)]*\);?", "", text).replace("::@", "").strip()
    if re.fullmatch(r"\$<LINK_ONLY:.*>", text):
        return None, []
    return resolve_target_reference(policy, text)


def resolve_inventory_path(policy: Policy, value: str) -> Path | None:
    atoms = unwrap_cmake_value(value)
    if len(atoms) != 1:
        return None
    text = atoms[0]
    text = text.replace("${CMAKE_SOURCE_DIR}", str(policy.source_root))
    text = text.replace("${PROJECT_SOURCE_DIR}", str(policy.source_root))
    text = text.replace("${CMAKE_BINARY_DIR}", str(policy.binary_root))
    if "$<" in text or "${" in text:
        return None
    path = Path(text)
    return path if path.is_absolute() else policy.source_root / path


def resolve_target_source(policy: Policy, target: Target, value: str) -> Path | None:
    """Resolve a target SOURCES atom using CMake's target source directory."""
    atoms = unwrap_cmake_value(value)
    if len(atoms) != 1:
        return None
    text = atoms[0]
    text = text.replace("${CMAKE_SOURCE_DIR}", str(policy.source_root))
    text = text.replace("${PROJECT_SOURCE_DIR}", str(policy.source_root))
    text = text.replace("${CMAKE_BINARY_DIR}", str(policy.binary_root))
    if "$<" in text or "${" in text:
        return None
    path = Path(text)
    return path if path.is_absolute() else Path(target.source_directory) / path


def include_module(policy: Policy, value: str) -> str | None:
    path = resolve_inventory_path(policy, value)
    if path is None:
        return None
    normalized = path.resolve()
    for module in policy.modules.values():
        module_root = (policy.root / module.path).resolve()
        for visibility in ("Include", "Private"):
            boundary = module_root / visibility
            try:
                normalized.relative_to(boundary)
                return module.id
            except ValueError:
                continue
    return None


def edge_allowed(policy: Policy, source: str, target: str) -> bool:
    return source == target or target in policy.modules[source].allowed_modules


def module_sources_by_prefix(policy: Policy, prefix: str) -> list[Path]:
    module = policy.module_for_prefix(prefix)
    if module is None:
        raise GateInputError(f"unknown manifest public include prefix: {prefix}")
    return policy.source_files([module])
