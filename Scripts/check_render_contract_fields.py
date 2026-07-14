#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


DEFAULT_ENTRIES = (
    "RenderContracts/Include/RenderContracts/RenderFramePacket.h:RenderFramePacket",
    "RenderContracts/Include/RenderContracts/ResourceUploadRequest.h:ResourceUploadRequest",
)

TOKEN_PATTERN = re.compile(
    r"::|&&|<=>|->|[A-Za-z_]\w*|0[xX][0-9A-Fa-f]+|\d+(?:\.\d*)?|"
    r"[{}()\[\];,:<>=*&~]"
)
QUOTED_INCLUDE_PATTERN = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.MULTILINE)
COMMENT_PATTERN = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)
PREPROCESSOR_PATTERN = re.compile(r"^\s*#.*$", re.MULTILINE)


class InputError(RuntimeError):
    pass


class ParseError(RuntimeError):
    pass


@dataclass(frozen=True)
class AggregateDecl:
    name: str
    bases: tuple[tuple[str, ...], ...]
    body: tuple[str, ...]
    source: Path


@dataclass(frozen=True)
class FieldDecl:
    name: str
    type_tokens: tuple[str, ...]
    forbidden_kind: str | None = None


@dataclass
class DeclarationIndex:
    aggregates: dict[str, AggregateDecl]
    enums: set[str]
    aliases: dict[str, tuple[str, ...]]


def canonical_name(tokens: Sequence[str]) -> str:
    text = "".join(tokens)
    return text.removeprefix("::")


def short_name(name: str) -> str:
    return name.split("::")[-1]


def strip_source(source: str) -> str:
    without_comments = COMMENT_PATTERN.sub("", source)
    return PREPROCESSOR_PATTERN.sub("", without_comments)


def tokenize(source: str) -> list[str]:
    return TOKEN_PATTERN.findall(strip_source(source))


def find_matching(tokens: Sequence[str], start: int, opening: str, closing: str) -> int:
    depth = 0
    for index in range(start, len(tokens)):
        if tokens[index] == opening:
            depth += 1
        elif tokens[index] == closing:
            depth -= 1
            if depth == 0:
                return index
    raise ParseError(f"unmatched {opening!r} token")


def resolve_include(root: Path, including: Path, include: str) -> Path:
    include_parts = Path(include).parts
    module_candidate = (
        root / include_parts[0] / "Include" / Path(*include_parts)
        if include_parts
        else root / include
    )
    direct_candidates = (including.parent / include, root / include, module_candidate)
    for candidate in direct_candidates:
        if candidate.is_file():
            return candidate.resolve()

    matches = []
    for candidate in root.rglob(include_parts[-1]):
        if not candidate.is_file():
            continue
        if tuple(candidate.parts[-len(include_parts):]) == include_parts:
            matches.append(candidate.resolve())
    unique_matches = sorted(set(matches))
    if len(unique_matches) == 1:
        return unique_matches[0]
    if not unique_matches:
        raise ParseError(
            f"missing quoted include: {include} included from {including}"
        )
    raise ParseError(
        f"ambiguous quoted include: {include} included from {including}"
    )


def load_include_graph(root: Path, entry_paths: Iterable[Path]) -> dict[Path, str]:
    loaded: dict[Path, str] = {}

    def load(path: Path) -> None:
        resolved = path.resolve()
        if resolved in loaded:
            return
        if not resolved.is_file():
            raise ParseError(f"missing contract root: {resolved}")
        try:
            source = resolved.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as error:
            raise ParseError(f"unable to read {resolved}: {error}") from error
        loaded[resolved] = source
        for include in QUOTED_INCLUDE_PATTERN.findall(source):
            load(resolve_include(root, resolved, include))

    for entry_path in entry_paths:
        load(entry_path)
    return loaded


def add_aggregate(
    aggregates: dict[str, AggregateDecl],
    declaration: AggregateDecl,
) -> None:
    existing = aggregates.get(declaration.name)
    if existing is not None and (
        existing.bases != declaration.bases or existing.body != declaration.body
    ):
        raise ParseError(
            "conflicting duplicate declaration: "
            f"{declaration.name} in {existing.source} and {declaration.source}"
        )
    aggregates[declaration.name] = declaration


def qualify_name(namespace: str, name: str) -> str:
    return f"{namespace}::{name}" if namespace else name


def parse_base_list(tokens: Sequence[str]) -> tuple[tuple[str, ...], ...]:
    if ":" not in tokens:
        return ()
    colon = tokens.index(":")
    base_tokens = tokens[colon + 1 :]
    bases: list[tuple[str, ...]] = []
    current: list[str] = []
    angle_depth = 0
    for token in base_tokens:
        if token == "<":
            angle_depth += 1
        elif token == ">":
            angle_depth -= 1
        if token == "," and angle_depth == 0:
            if current:
                bases.append(tuple(current))
            current = []
        else:
            current.append(token)
    if current:
        bases.append(tuple(current))

    ignored_specifiers = {"public", "protected", "private", "virtual"}
    normalized = []
    for base in bases:
        value = tuple(token for token in base if token not in ignored_specifiers)
        if not value:
            raise ParseError("empty base declaration")
        normalized.append(value)
    return tuple(normalized)


def scan_aggregates_and_enums(
    files: dict[Path, str],
) -> tuple[dict[str, AggregateDecl], set[str]]:
    aggregates: dict[str, AggregateDecl] = {}
    enums: set[str] = set()

    for path, source in files.items():
        tokens = tokenize(source)

        def scan_scope(start: int, end: int, namespace: str) -> None:
            index = start
            while index < end:
                token = tokens[index]
                if token == "namespace":
                    cursor = index + 1
                    while cursor < end and tokens[cursor] not in ("{", ";", "="):
                        cursor += 1
                    if cursor < end and tokens[cursor] == "{":
                        close = find_matching(tokens, cursor, "{", "}")
                        nested_name = canonical_name(tokens[index + 1 : cursor])
                        nested_namespace = (
                            qualify_name(namespace, nested_name)
                            if nested_name
                            else namespace
                        )
                        scan_scope(cursor + 1, close, nested_namespace)
                        index = close + 1
                        continue
                if token == "enum":
                    cursor = index + 1
                    if cursor < end and tokens[cursor] in ("class", "struct"):
                        cursor += 1
                    if cursor < end and re.match(r"^[A-Za-z_]", tokens[cursor]):
                        name = tokens[cursor]
                        while cursor < end and tokens[cursor] not in ("{", ";"):
                            cursor += 1
                        if cursor < end and tokens[cursor] == "{":
                            qualified = qualify_name(namespace, name)
                            enums.add(qualified)
                            index = find_matching(tokens, cursor, "{", "}") + 1
                            continue
                if token in ("class", "struct") and not (
                    index > start and tokens[index - 1] == "enum"
                ):
                    if index + 1 >= end:
                        raise ParseError(f"incomplete {token} declaration in {path}")
                    name = tokens[index + 1]
                    cursor = index + 2
                    while cursor < end and tokens[cursor] not in ("{", ";"):
                        cursor += 1
                    if cursor < end and tokens[cursor] == "{":
                        close = find_matching(tokens, cursor, "{", "}")
                        bases = parse_base_list(tokens[index + 2 : cursor])
                        add_aggregate(
                            aggregates,
                            AggregateDecl(
                                qualify_name(namespace, name),
                                bases,
                                tuple(tokens[cursor + 1 : close]),
                                path,
                            ),
                        )
                        index = close + 1
                        continue
                index += 1

        scan_scope(0, len(tokens), "")
    return aggregates, enums


def scan_aliases(files: dict[Path, str]) -> dict[str, tuple[str, ...]]:
    aliases: dict[str, tuple[str, ...]] = {}

    def add_alias(name: str, value: tuple[str, ...], path: Path) -> None:
        existing = aliases.get(name)
        if existing is not None and existing != value:
            raise ParseError(
                f"conflicting duplicate declaration: alias {name} in {path}"
            )
        aliases[name] = value

    for path, source in files.items():
        tokens = tokenize(source)

        def scan_scope(start: int, end: int, namespace: str) -> None:
            index = start
            while index < end:
                if tokens[index] == "namespace":
                    cursor = index + 1
                    while cursor < end and tokens[cursor] not in ("{", ";", "="):
                        cursor += 1
                    if cursor < end and tokens[cursor] == "{":
                        close = find_matching(tokens, cursor, "{", "}")
                        nested_name = canonical_name(tokens[index + 1 : cursor])
                        nested_namespace = (
                            qualify_name(namespace, nested_name)
                            if nested_name
                            else namespace
                        )
                        scan_scope(cursor + 1, close, nested_namespace)
                        index = close + 1
                        continue
                if tokens[index] == "using" and index + 2 < end:
                    name = tokens[index + 1]
                    if tokens[index + 2] == "=":
                        cursor = index + 3
                        angle_depth = 0
                        while cursor < end:
                            if tokens[cursor] == "<":
                                angle_depth += 1
                            elif tokens[cursor] == ">":
                                angle_depth -= 1
                            elif tokens[cursor] == ";" and angle_depth == 0:
                                break
                            cursor += 1
                        if cursor >= end:
                            raise ParseError(f"unterminated using alias {name} in {path}")
                        add_alias(
                            qualify_name(namespace, name),
                            tuple(tokens[index + 3 : cursor]),
                            path,
                        )
                        index = cursor + 1
                        continue
                if tokens[index] == "typedef":
                    cursor = index + 1
                    while cursor < end and tokens[cursor] != ";":
                        cursor += 1
                    declaration = tokens[index + 1 : cursor]
                    if declaration:
                        name = declaration[-1]
                        add_alias(
                            qualify_name(namespace, name),
                            tuple(declaration[:-1]),
                            path,
                        )
                    index = cursor + 1
                    continue
                index += 1

        scan_scope(0, len(tokens), "")
    return aliases


def build_index(files: dict[Path, str]) -> DeclarationIndex:
    aggregates, enums = scan_aggregates_and_enums(files)
    aliases = scan_aliases(files)
    return DeclarationIndex(aggregates, enums, aliases)


def split_statements(body: Sequence[str]) -> list[list[str]]:
    statements: list[list[str]] = []
    current: list[str] = []
    index = 0
    while index < len(body):
        token = body[index]
        if token == "{" and "(" in current:
            end = find_matching(body, index, "{", "}")
            current.clear()
            index = end + 1
            continue
        if token == "{":
            end = find_matching(body, index, "{", "}")
            current.extend(body[index : end + 1])
            index = end + 1
            continue
        if token == ";":
            if current:
                statements.append(current)
            current = []
            index += 1
            continue
        current.append(token)
        index += 1
    return statements


def initializer_prefix(statement: Sequence[str]) -> list[str]:
    angle_depth = 0
    bracket_depth = 0
    for index, token in enumerate(statement):
        if token == "<":
            angle_depth += 1
        elif token == ">":
            angle_depth = max(0, angle_depth - 1)
        elif token == "[":
            bracket_depth += 1
        elif token == "]":
            bracket_depth = max(0, bracket_depth - 1)
        elif (
            angle_depth == 0
            and bracket_depth == 0
            and token in ("=", "{")
            and not (token == "=" and index > 0 and statement[index - 1] == "operator")
        ):
            return list(statement[:index])
    return list(statement)


def is_parenthesized_declarator(tokens: Sequence[str]) -> bool:
    if not tokens or "(" in tokens:
        return False
    if tokens[0] in ("*", "&", "&&"):
        return True
    try:
        operator_index = tokens.index("*")
    except ValueError:
        return False
    qualifier = tokens[:operator_index]
    if len(qualifier) < 2 or qualifier[-1] != "::":
        return False
    for index, token in enumerate(qualifier):
        if index % 2 == 0 and not re.match(r"^[A-Za-z_]", token):
            return False
        if index % 2 == 1 and token != "::":
            return False
    return True


def parse_field(statement: Sequence[str]) -> FieldDecl | None:
    tokens = list(statement)
    while len(tokens) >= 2 and tokens[0] in ("public", "private", "protected") and tokens[1] == ":":
        tokens = tokens[2:]
    if (
        not tokens
        or tokens[0] in ("friend", "using", "typedef", "template")
        or "static" in tokens
    ):
        return None

    prefix = initializer_prefix(tokens)
    if not prefix:
        return None
    first_parenthesis = None
    angle_depth = 0
    for index, token in enumerate(prefix):
        if token == "<":
            angle_depth += 1
        elif token == ">":
            angle_depth = max(0, angle_depth - 1)
        elif token == "(" and angle_depth == 0:
            first_parenthesis = index
            break
    if first_parenthesis is not None:
        first_closing = find_matching(prefix, first_parenthesis, "(", ")")
        first_declarator = prefix[first_parenthesis + 1 : first_closing]
        if not is_parenthesized_declarator(first_declarator):
            return None
    angle_depth = 0
    for index, token in enumerate(prefix):
        if token == "<":
            angle_depth += 1
            continue
        if token == ">":
            angle_depth = max(0, angle_depth - 1)
            continue
        if token != "(" or angle_depth != 0:
            continue
        closing = find_matching(prefix, index, "(", ")")
        declarator = prefix[index + 1 : closing]
        declarator_operators = [
            (operator_index, part)
            for operator_index, part in enumerate(declarator)
            if part in ("*", "&", "&&")
        ]
        operator_index, operator_token = (
            declarator_operators[-1] if declarator_operators else (-1, "")
        )
        has_parenthesized_declarator = (
            index == first_parenthesis
            and is_parenthesized_declarator(declarator)
        )
        is_function_pointer = (
            operator_token == "*"
            and "(" not in declarator
            and closing + 1 < len(prefix)
            and prefix[closing + 1] == "("
        )
        if has_parenthesized_declarator or is_function_pointer:
            after_operator = declarator[operator_index + 1 :]
            qualifiers = {"const", "volatile", "restrict", "__restrict", "__restrict__"}
            names = [
                part
                for part in after_operator
                if re.match(r"^[A-Za-z_]", part) and part not in qualifiers
            ]
            field_name = names[-1] if names else "<parenthesized-declarator>"
            is_function = (
                closing + 1 < len(prefix) and prefix[closing + 1] == "("
            )
            if operator_token in ("&", "&&"):
                forbidden_kind = "reference"
            elif is_function:
                forbidden_kind = "function pointer"
            else:
                forbidden_kind = "raw pointer"
            return FieldDecl(field_name, tuple(prefix), forbidden_kind)
    angle_depth = 0
    has_top_level_parenthesis = False
    for token in prefix:
        if token == "<":
            angle_depth += 1
        elif token == ">":
            angle_depth = max(0, angle_depth - 1)
        elif token == "(" and angle_depth == 0:
            has_top_level_parenthesis = True
            break
    if has_top_level_parenthesis:
        return None

    while prefix and prefix[-1] == "]":
        opening = len(prefix) - 1
        depth = 1
        opening -= 1
        while opening >= 0:
            if prefix[opening] == "]":
                depth += 1
            elif prefix[opening] == "[":
                depth -= 1
                if depth == 0:
                    break
            opening -= 1
        prefix = prefix[:opening]
    if not prefix:
        return None

    name_index = len(prefix) - 1
    while name_index >= 0 and not re.match(r"^[A-Za-z_]", prefix[name_index]):
        name_index -= 1
    if name_index <= 0:
        return None
    name = prefix[name_index]
    type_tokens = prefix[:name_index]
    if "*" in type_tokens:
        return FieldDecl(name, tuple(type_tokens), "raw pointer")
    if "&" in type_tokens or "&&" in type_tokens:
        return FieldDecl(name, tuple(type_tokens), "reference")
    type_tokens = [
        token for token in type_tokens if token not in ("const", "volatile", "mutable")
    ]
    if not type_tokens:
        return None
    return FieldDecl(name, tuple(type_tokens))


def aggregate_fields(declaration: AggregateDecl) -> list[FieldDecl]:
    fields = []
    for statement in split_statements(declaration.body):
        field = parse_field(statement)
        if field is not None:
            fields.append(field)
    return fields


def split_template_arguments(tokens: Sequence[str]) -> list[tuple[str, ...]]:
    arguments: list[tuple[str, ...]] = []
    current: list[str] = []
    depth = 0
    for token in tokens:
        if token == "<":
            depth += 1
        elif token == ">":
            depth -= 1
        if token == "," and depth == 0:
            arguments.append(tuple(current))
            current = []
        else:
            current.append(token)
    if current:
        arguments.append(tuple(current))
    return arguments


def outer_template(tokens: Sequence[str]) -> tuple[str, list[tuple[str, ...]]] | None:
    try:
        opening = tokens.index("<")
    except ValueError:
        return None
    if not tokens or tokens[-1] != ">":
        raise ParseError(f"malformed reachable template type: {canonical_name(tokens)}")
    base = canonical_name(tokens[:opening])
    arguments = split_template_arguments(tokens[opening + 1 : -1])
    return base, arguments


FUNDAMENTAL_LEAVES = {
    "bool",
    "char",
    "signedchar",
    "unsignedchar",
    "short",
    "unsignedshort",
    "int",
    "unsignedint",
    "long",
    "unsignedlong",
    "longlong",
    "unsignedlonglong",
    "float",
    "double",
    "longdouble",
    "size_t",
    "std::size_t",
    "int8",
    "uint8",
    "int16",
    "uint16",
    "int32",
    "uint32",
    "int64",
    "uint64",
    "float32",
    "float64",
    "std::int8_t",
    "std::uint8_t",
    "std::int16_t",
    "std::uint16_t",
    "std::int32_t",
    "std::uint32_t",
    "std::int64_t",
    "std::uint64_t",
}
MATH_LEAVES = {
    "glm::vec2",
    "glm::vec3",
    "glm::vec4",
    "glm::mat3",
    "glm::mat4",
    "glm::quat",
}
OWNING_TEMPLATES = {
    "std::vector",
    "std::array",
    "std::optional",
    "std::variant",
}
FORBIDDEN_TEMPLATES = {
    "std::function",
    "std::span",
    "std::string_view",
    "std::reference_wrapper",
    "std::unique_ptr",
    "std::shared_ptr",
    "std::weak_ptr",
}


def forbidden_named_type(name: str) -> str | None:
    lowered = name.lower()
    if name in FORBIDDEN_TEMPLATES:
        return name
    if short_name(name) == "Ref" or name in ("Core::Ref", "RVX::Ref"):
        return "Ref"
    backend_fragments = ("rhi", "dx11", "dx12", "vulkan", "metal", "opengl")
    if any(fragment in lowered for fragment in backend_fragments):
        return f"RHI/backend type {name}"
    return None


class ContractWalker:
    def __init__(self, index: DeclarationIndex) -> None:
        self.index = index
        self.field_count = 0
        self.errors: list[str] = []

    def walk_root(self, root_type: str) -> None:
        self._walk_type((root_type,), root_type, ())

    def _declaration_kinds(self, name: str) -> list[str]:
        kinds = []
        if name in self.index.aliases:
            kinds.append("alias")
        if name in self.index.enums:
            kinds.append("enum")
        if name in self.index.aggregates:
            kinds.append("aggregate")
        return kinds

    def _resolve_named_type(
        self,
        name: str,
        context_namespace: str,
    ) -> tuple[str | None, str | None]:
        all_names = (
            set(self.index.aggregates)
            | self.index.enums
            | set(self.index.aliases)
        )

        def resolve_exact(exact_name: str) -> tuple[str | None, str | None]:
            kinds = self._declaration_kinds(exact_name)
            if len(kinds) == 1:
                return exact_name, kinds[0]
            if len(kinds) > 1:
                return None, "ambiguous"
            return None, None

        if "::" in name:
            return resolve_exact(name)

        if context_namespace:
            contextual_name = qualify_name(context_namespace, name)
            if contextual_name in all_names:
                return resolve_exact(contextual_name)
        if name in all_names:
            return resolve_exact(name)

        short_matches = sorted(
            candidate for candidate in all_names if short_name(candidate) == name
        )
        if len(short_matches) == 1:
            return resolve_exact(short_matches[0])
        if len(short_matches) > 1:
            return None, "ambiguous"
        return None, None

    def _walk_type(
        self,
        raw_tokens: Sequence[str],
        path: str,
        aggregate_stack: tuple[str, ...],
        alias_stack: tuple[str, ...] = (),
        context_namespace: str = "",
    ) -> None:
        tokens = tuple(token for token in raw_tokens if token not in ("const", "volatile"))
        if "*" in tokens:
            self.errors.append(f"raw pointer via {path}")
            return
        if "&" in tokens or "&&" in tokens:
            self.errors.append(f"reference via {path}")
            return

        template = outer_template(tokens)
        if template is not None:
            base, arguments = template
            forbidden = forbidden_named_type(base)
            if forbidden is not None:
                self.errors.append(f"forbidden {forbidden} via {path}")
                return
            if base not in OWNING_TEMPLATES:
                self.errors.append(f"unresolved reachable type: {base} via {path}")
                return
            traversed_arguments = arguments[:1] if base == "std::array" else arguments
            for argument in traversed_arguments:
                self._walk_type(
                    argument,
                    path,
                    aggregate_stack,
                    alias_stack,
                    context_namespace,
                )
            return

        name = canonical_name(tokens)
        forbidden = forbidden_named_type(name)
        if forbidden is not None:
            self.errors.append(f"forbidden {forbidden} via {path}")
            return
        if name in FUNDAMENTAL_LEAVES or name == "std::string" or name in MATH_LEAVES:
            return

        resolved_name, declaration_kind = self._resolve_named_type(
            name,
            context_namespace,
        )
        if declaration_kind == "ambiguous":
            self.errors.append(f"ambiguous reachable type: {name} via {path}")
            return
        if resolved_name is None or declaration_kind is None:
            self.errors.append(f"unresolved reachable type: {name} via {path}")
            return

        if declaration_kind == "alias":
            alias = self.index.aliases[resolved_name]
            if resolved_name in alias_stack:
                self.errors.append(f"recursive alias: {resolved_name} via {path}")
                return
            alias_namespace = resolved_name.rpartition("::")[0]
            self._walk_type(
                alias,
                path,
                aggregate_stack,
                (*alias_stack, resolved_name),
                alias_namespace,
            )
            return
        if declaration_kind == "enum":
            return

        declaration = self.index.aggregates[resolved_name]
        if declaration.name in aggregate_stack:
            return
        next_stack = (*aggregate_stack, declaration.name)
        declaration_namespace = declaration.name.rpartition("::")[0]
        for base in declaration.bases:
            base_name = canonical_name(base)
            self._walk_type(
                base,
                f"{path}.<base:{base_name}>",
                next_stack,
                alias_stack,
                declaration_namespace,
            )
        for field in aggregate_fields(declaration):
            self.field_count += 1
            field_path = f"{path}.{field.name}"
            if field.forbidden_kind is not None:
                self.errors.append(f"forbidden {field.forbidden_kind} via {field_path}")
                continue
            self._walk_type(
                field.type_tokens,
                field_path,
                next_stack,
                alias_stack,
                declaration_namespace,
            )


def parse_entry(root: Path, raw_entry: str) -> tuple[Path, str]:
    if ":" not in raw_entry:
        raise InputError(f"entry must be HEADER:TYPE: {raw_entry}")
    raw_header, root_type = raw_entry.rsplit(":", 1)
    if not raw_header or not root_type or not re.fullmatch(r"[A-Za-z_]\w*", root_type):
        raise InputError(f"entry must be HEADER:TYPE: {raw_entry}")
    header = Path(raw_header)
    if not header.is_absolute():
        header = root / header
    if not header.is_file():
        raise InputError(f"contract root header does not exist: {header}")
    return header.resolve(), root_type


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Fail-closed owned-value checker for render contract fields."
    )
    parser.add_argument(
        "--root",
        "--repo-root",
        dest="root",
        required=True,
        help="Repository or isolated include-graph root.",
    )
    parser.add_argument(
        "--entry",
        "--contract",
        dest="entries",
        action="append",
        default=[],
        help="Root declaration in HEADER:TYPE form; may be repeated.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = Path(args.root).resolve()
    if not root.is_dir():
        raise InputError(f"repository root does not exist: {root}")
    raw_entries = args.entries or list(DEFAULT_ENTRIES)
    entries = [parse_entry(root, raw_entry) for raw_entry in raw_entries]
    files = load_include_graph(root, (entry[0] for entry in entries))
    index = build_index(files)

    failed = False
    for header, root_type in entries:
        walker = ContractWalker(index)
        walker.walk_root(root_type)
        if walker.errors:
            failed = True
            for error in walker.errors:
                print(error, file=sys.stderr)
        try:
            display_header = header.relative_to(root)
        except ValueError:
            display_header = header
        print(
            f"Scanned {display_header}:{root_type} - "
            f"reachable instance fields: {walker.field_count}; "
            f"forbidden fields: {len(walker.errors)}"
        )
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except InputError as error:
        print(f"Render contract field checker input error: {error}", file=sys.stderr)
        raise SystemExit(2)
    except ParseError as error:
        print(f"Render contract field checker parse error: {error}", file=sys.stderr)
        raise SystemExit(2)
