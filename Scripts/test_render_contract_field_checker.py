#!/usr/bin/env python3
from __future__ import annotations

import re
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
CHECKER = REPO_ROOT / "Scripts" / "check_render_contract_fields.py"


def write_header(root: Path, relative_path: str, source: str) -> None:
    path = root / relative_path
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(textwrap.dedent(source), encoding="utf-8")


def run_checker(
    root: Path,
    *entries: str,
) -> subprocess.CompletedProcess[str]:
    command = [sys.executable, str(CHECKER), "--root", str(root)]
    for entry in entries:
        command.extend(("--entry", entry))
    return subprocess.run(
        command,
        cwd=REPO_ROOT,
        text=True,
        capture_output=True,
        check=False,
    )


class RenderContractFieldCheckerTests(unittest.TestCase):
    def assert_forbidden(
        self,
        root: Path,
        expected: str,
        entry: str = "Root.h:Root",
    ) -> None:
        result = run_checker(root, entry)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(expected.lower(), (result.stdout + result.stderr).lower())

    def test_legal_recursive_owned_graph_passes(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_header(
                root,
                "Leaf.h",
                """
                #pragma once
                #include <string>
                struct Leaf { std::string label; int value = 0; };
                """,
            )
            write_header(
                root,
                "Middle.h",
                """
                #pragma once
                #include "Leaf.h"
                #include <array>
                #include <optional>
                #include <variant>
                struct Middle {
                    std::variant<Leaf, std::optional<std::array<Leaf, 2>>> value;
                };
                """,
            )
            write_header(
                root,
                "Root.h",
                """
                #pragma once
                #include "Middle.h"
                #include <vector>
                class Root { private: std::vector<Middle> values; };
                """,
            )

            result = run_checker(root, "Root.h:Root")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertRegex(result.stdout, r"reachable instance fields:\s*[1-9][0-9]*")
            self.assertIn("forbidden fields: 0", result.stdout)

    def test_raw_pointer_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_header(root, "Root.h", "struct Root { int* ptr; };")
            self.assert_forbidden(root, "raw pointer")

    def test_reference_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_header(root, "Root.h", "struct Root { int& ref; };")
            self.assert_forbidden(root, "reference")

    def test_function_pointer_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_header(root, "Root.h", "struct Root { void (*callback)(int); };")
            self.assert_forbidden(root, "function pointer")

    def test_qualified_function_pointer_forms_fail(self) -> None:
        declarations = (
            "void (*const callback)(int);",
            "void (*volatile callback)(int);",
            "void (*const volatile callback)(int);",
            "void (*callback)(int) noexcept;",
        )
        for declaration in declarations:
            with self.subTest(declaration=declaration), tempfile.TemporaryDirectory() as temp_dir:
                root = Path(temp_dir)
                write_header(root, "Root.h", f"struct Root {{ {declaration} }};")
                self.assert_forbidden(root, "function pointer")

    def test_parenthesized_object_pointer_and_function_reference_fail(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_header(
                root,
                "Root.h",
                "struct Root { int (*const ptr); void (&callback)(int); int value; };",
            )

            result = run_checker(root, "Root.h:Root")
            output = result.stdout + result.stderr
            self.assertNotEqual(result.returncode, 0, output)
            self.assertIn("forbidden raw pointer via Root.ptr", output)
            self.assertIn("forbidden reference via Root.callback", output)
            self.assertIn("reachable instance fields: 3", result.stdout)

    def test_parenthesized_cv_and_member_pointer_forms_fail(self) -> None:
        declarations = (
            ("int (*const volatile ptr);", "raw pointer"),
            ("int (Root::*const ptr);", "raw pointer"),
            ("void (Root::*const callback)(int);", "function pointer"),
        )
        for declaration, expected in declarations:
            with self.subTest(declaration=declaration), tempfile.TemporaryDirectory() as temp_dir:
                root = Path(temp_dir)
                write_header(root, "Root.h", f"struct Root {{ {declaration} }};")
                self.assert_forbidden(root, expected)

    def test_callback_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_header(
                root,
                "Root.h",
                "#include <functional>\nstruct Root { std::function<void()> callback; };",
            )
            self.assert_forbidden(root, "std::function")

    def test_span_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_header(
                root,
                "Root.h",
                "#include <span>\nstruct Root { std::span<int> values; };",
            )
            self.assert_forbidden(root, "std::span")

    def test_string_view_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_header(
                root,
                "Root.h",
                "#include <string_view>\nstruct Root { std::string_view text; };",
            )
            self.assert_forbidden(root, "std::string_view")

    def test_ref_alias_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_header(root, "Root.h", "struct Root { Ref<int> value; };")
            self.assert_forbidden(root, "Ref")

    def test_shared_pointer_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_header(
                root,
                "Root.h",
                "#include <memory>\nstruct Root { std::shared_ptr<int> value; };",
            )
            self.assert_forbidden(root, "std::shared_ptr")

    def test_rhi_alias_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_header(root, "Aliases.h", "using TextureAlias = RHITextureRef;")
            write_header(
                root,
                "Root.h",
                '#include "Aliases.h"\nstruct Root { TextureAlias texture; };',
            )
            self.assert_forbidden(root, "RHI")

    def test_forbidden_second_level_aggregate_field_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_header(
                root,
                "Nested.h",
                """
                #include <span>
                struct Inner { std::span<int> borrowed; };
                struct Outer { Inner inner; };
                """,
            )
            write_header(
                root,
                "Root.h",
                '#include "Nested.h"\nstruct Root { Outer outer; };',
            )
            self.assert_forbidden(root, "std::span")
            self.assert_forbidden(root, "Root.outer.inner.borrowed")

    def test_inherited_instance_fields_are_traversed(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_header(root, "Base.h", "struct Base { int* ptr; };")
            write_header(
                root,
                "Root.h",
                '#include "Base.h"\nstruct Root : public Base { int value; };',
            )
            self.assert_forbidden(root, "raw pointer")

    def test_unresolved_base_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_header(root, "Root.h", "struct Root : MissingBase { int value; };")
            self.assert_forbidden(root, "unresolved reachable type: MissingBase")

    def test_inheritance_cycle_is_bounded_and_still_traverses_fields(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_header(
                root,
                "Root.h",
                """
                struct B;
                struct A : B { int value; };
                struct B : A { int* ptr; };
                struct Root : A {};
                """,
            )
            self.assert_forbidden(root, "raw pointer")

    def test_alias_to_pointer_in_another_header_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_header(root, "Aliases.h", "using BorrowedValue = int*;")
            write_header(
                root,
                "Root.h",
                '#include "Aliases.h"\nstruct Root { BorrowedValue value; };',
            )
            self.assert_forbidden(root, "raw pointer")

    def test_unresolved_reachable_type_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_header(root, "Root.h", "struct Root { Mystery value; };")
            self.assert_forbidden(root, "unresolved reachable type: Mystery")
            self.assert_forbidden(root, "Root.value")

    def test_qualified_unresolved_type_does_not_fallback_to_short_name(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_header(
                root,
                "Root.h",
                """
                struct Safe { int value; };
                struct Root { Missing::Safe value; };
                """,
            )
            self.assert_forbidden(root, "unresolved reachable type: Missing::Safe")

    def test_qualified_declared_types_resolve_exactly(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_header(
                root,
                "Root.h",
                """
                namespace Present {
                    enum class Mode : int { Default = 0 };
                    using Count = int;
                    struct Safe { Count value; };
                }
                struct Root {
                    Present::Safe safe;
                    Present::Mode mode = Present::Mode::Default;
                    Present::Count count = 0;
                };
                """,
            )
            result = run_checker(root, "Root.h:Root")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_same_short_names_resolve_by_exact_qualified_name(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_header(
                root,
                "Root.h",
                """
                namespace A {
                    enum class Mode : int { Default = 0 };
                    using Count = int;
                    struct Value { Count count; Mode mode = Mode::Default; };
                }
                namespace B {
                    enum class Mode : int { Default = 0 };
                    using Count = float;
                    struct Value { Count count; Mode mode = Mode::Default; };
                }
                struct Root {
                    A::Value aValue;
                    B::Value bValue;
                    A::Mode aMode = A::Mode::Default;
                    B::Mode bMode = B::Mode::Default;
                    A::Count aCount = 0;
                    B::Count bCount = 0;
                };
                """,
            )

            result = run_checker(root, "Root.h:Root")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("forbidden fields: 0", result.stdout)

    def test_ambiguous_unqualified_types_fail_only_when_reachable(self) -> None:
        declarations = (
            (
                "namespace A { struct Value { int value; }; } "
                "namespace B { struct Value { float value; }; }",
                "Value",
            ),
            (
                "namespace A { enum class Mode : int { Default = 0 }; } "
                "namespace B { enum class Mode : int { Default = 0 }; }",
                "Mode",
            ),
            (
                "namespace A { using Count = int; } "
                "namespace B { using Count = float; }",
                "Count",
            ),
        )
        for declarations_source, field_type in declarations:
            with self.subTest(field_type=field_type), tempfile.TemporaryDirectory() as temp_dir:
                root = Path(temp_dir)
                write_header(
                    root,
                    "Root.h",
                    f"{declarations_source} struct Root {{ {field_type} value; }};",
                )
                self.assert_forbidden(
                    root,
                    f"ambiguous reachable type: {field_type} via Root.value",
                )

    def test_unique_unqualified_short_names_still_resolve(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_header(
                root,
                "Root.h",
                """
                namespace Only {
                    struct Value { int value; };
                    enum class Mode : int { Default = 0 };
                    using Count = int;
                }
                struct Root { Value value; Mode mode; Count count; };
                """,
            )

            result = run_checker(root, "Root.h:Root")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("forbidden fields: 0", result.stdout)

    def test_const_reference_accessor_is_ignored(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_header(
                root,
                "Root.h",
                """
                struct Forbidden;
                class Root {
                public:
                    const Forbidden& GetForbidden() const;
                private:
                    int value = 0;
                };
                """,
            )
            result = run_checker(root, "Root.h:Root")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_method_function_pointer_parameter_is_ignored(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_header(
                root,
                "Root.h",
                "struct Root { void Set(void (*callback)(int)); int value; };",
            )

            result = run_checker(root, "Root.h:Root")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("reachable instance fields: 1", result.stdout)
            self.assertIn("forbidden fields: 0", result.stdout)

    def test_missing_quoted_include_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_header(
                root,
                "Root.h",
                '#include "Missing.h"\nstruct Root { int value; };',
            )
            self.assert_forbidden(root, "missing quoted include")

    def test_conflicting_duplicate_declarations_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_header(root, "One.h", "struct Duplicate { int first; };")
            write_header(root, "Two.h", "struct Duplicate { float second; };")
            write_header(
                root,
                "Root.h",
                '#include "One.h"\n#include "Two.h"\nstruct Root { Duplicate value; };',
            )
            self.assert_forbidden(root, "conflicting duplicate declaration")

    def test_custom_template_value_is_substituted_and_traversed(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_header(
                root,
                "Root.h",
                """
                #include <optional>
                template <typename State>
                struct Wrapper { State state; };
                struct Leaf { int value; };
                struct Root { std::optional<Wrapper<Leaf>> value; };
                """,
            )
            result = run_checker(root, "Root.h:Root")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("reachable instance fields: 3", result.stdout)

    def test_custom_template_forbidden_argument_still_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_header(
                root,
                "Root.h",
                """
                template <typename State>
                struct Wrapper { State state; };
                struct Root { Wrapper<int*> value; };
                """,
            )
            self.assert_forbidden(root, "raw pointer")

    def test_missing_root_and_bad_cli_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            missing = Path(temp_dir) / "missing"
            result = run_checker(missing, "Root.h:Root")
            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
            bad_entry = run_checker(Path(temp_dir), "not-an-entry")
            self.assertEqual(bad_entry.returncode, 2, bad_entry.stdout + bad_entry.stderr)

    def test_real_render_contract_roots_are_owned_values(self) -> None:
        result = run_checker(
            REPO_ROOT,
            "RenderContracts/Include/RenderContracts/RenderFrameTypes.h:RenderFrameSettings",
            "RenderContracts/Include/RenderContracts/RenderFramePacketV5.h:RenderFrameHeaderV5",
            "RenderContracts/Include/RenderContracts/RenderFramePacketV5.h:RenderFramePacketV5",
            "RenderContracts/Include/RenderContracts/RenderSceneUpdate.h:RenderSceneUpdateBatch",
            "RenderContracts/Include/RenderContracts/ResourceUploadRequest.h:ResourceUploadRequest",
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        counts = [
            int(value)
            for value in re.findall(r"reachable instance fields:\s*([0-9]+)", result.stdout)
        ]
        self.assertEqual(len(counts), 5, result.stdout)
        self.assertTrue(all(count > 0 for count in counts), result.stdout)
        self.assertEqual(result.stdout.count("forbidden fields: 0"), 5, result.stdout)


if __name__ == "__main__":
    unittest.main()
