import importlib.util,tempfile,unittest
from pathlib import Path
from unittest.mock import patch
HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('fixture',HERE/'auto-rsx-emitter-fixture.py');fixture=importlib.util.module_from_spec(spec);spec.loader.exec_module(fixture)
class Fixture(unittest.TestCase):
    def test_changed_source_refused(self):
        text=fixture.SOURCE.read_text(encoding='utf-8').replace('u32 clear_mask)','u32 clear_mask) /* changed */',1)
        with tempfile.TemporaryDirectory() as d,patch.object(Path,'read_text',return_value=text):
            with self.assertRaises(ValueError):fixture.write(Path(d)/'fixture.h')
    def test_checked_boundaries(self):
        with self.assertRaises(AssertionError):fixture.extract('','missing')
        with self.assertRaises(ValueError):fixture.extract('func(){','func()')
if __name__=='__main__':unittest.main()
