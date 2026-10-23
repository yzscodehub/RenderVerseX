"""Exercise immutable candidate paths without invoking a GPU producer."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CandidateRunTests(unittest.TestCase):
    def run_candidate(self, root, run_id):
        env = dict(os.environ)
        env.update(RVX_VALIDATION_RUN_ID=run_id,
                   RVX_VALIDATION_SOURCE_COMMIT='a' * 40,
                   RVX_VALIDATION_PRESET='test')
        args = ['cmake',
                '-DRVX_CANDIDATE_ALLOWED_ROOT=' + root.as_posix(),
                '-DRVX_CANDIDATE_ROOT=' + (root / 'case').as_posix(),
                '-DRVX_CANDIDATE_EXECUTABLE=' + (root / 'missing-producer').as_posix(),
                '-DRVX_CANDIDATE_WORKING_DIRECTORY=' + root.as_posix(),
                '-DRVX_CANDIDATE_COOKED_SHADER_ROOT=' + root.as_posix(),
                '-DRVX_CANDIDATE_SAMPLE_ID=model-viewer',
                '-DRVX_CANDIDATE_BACKEND=dx12',
                '-DRVX_CANDIDATE_PIPELINE=deferred',
                '-DRVX_CANDIDATE_RENDER_PATH=direct',
                '-P', str(ROOT / 'Tests/CMake/RunFreshGfx25Candidate.cmake')]
        return subprocess.run(args, env=env, capture_output=True, text=True, timeout=15)

    def test_failed_run_is_preserved_and_cannot_be_reused(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            old = root / 'case' / 'runs' / 'old'
            old.mkdir(parents=True)
            marker = old / 'failure.log'
            marker.write_text('preserve failure', encoding='utf-8')
            reused = self.run_candidate(root, 'old')
            self.assertNotEqual(reused.returncode, 0)
            self.assertIn('already exists', reused.stderr)
            fresh = self.run_candidate(root, 'new')
            self.assertNotEqual(fresh.returncode, 0)
            self.assertTrue((root / 'case/runs/new').is_dir())
            self.assertEqual(marker.read_text(encoding='utf-8'), 'preserve failure')

    def test_invalid_identity_has_no_directory_side_effects(self):
        for run_id in ('', '../escape', 'a/b', '.', 'x' * 129):
            with self.subTest(run_id=run_id), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                result = self.run_candidate(root, run_id)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse((root / 'case').exists())


if __name__ == '__main__':
    unittest.main()
