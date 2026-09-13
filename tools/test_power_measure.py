import unittest
import power_measure as pm


class ParseTest(unittest.TestCase):
    def test_pwr_line(self):
        s = pm.parse_pwr("PWR t=12345 vbat=4123 vbus=5010 lvl=87 chg=0 disp=dim bri=35 conn=1 adv=0 cpu=240 "
                         "hold=1 rec=0")
        self.assertEqual(s["vbat"], 4123)
        self.assertEqual(s["disp"], "dim")
        self.assertEqual(s["hold"], 1)

    def test_pwr_after_other_output(self):
        self.assertEqual(pm.parse_pwr("garbage PWR t=1 vbat=-1")["vbat"], -1)

    def test_pwr_absent(self):
        self.assertIsNone(pm.parse_pwr("I (1) MuteApp: STATUS conn=1"))

    def test_drain_record(self):
        self.assertEqual(pm.parse_drain("DRAIN t=60 vbat=4100 st=5"), {"t": 60, "vbat": 4100, "st": 5})

    def test_drain_control_lines(self):
        for line in ("DRAIN end n=12", "DRAIN start interval=60s capacity=1440 nvs_interval=300s nvs_capacity=96",
                     "DRAIN stop n=3 nvs_n=1", "DRAIN source=nvs interval=300s", "DRAIN nvs_error=ESP_FAIL",
                     "DRAIN t=60"):
            self.assertIsNone(pm.parse_drain(line), line)

    def test_describe_state(self):
        self.assertEqual(pm.describe_state(0b000101), "conn+disp")
        self.assertEqual(pm.describe_state(0b110000), "chg+usb")
        self.assertEqual(pm.describe_state(0), "-")


class ReaderTest(unittest.TestCase):
    def test_routes_lines(self):
        reader = pm.Reader(port=None, echo=False)
        reader.handle("PWR t=1 vbat=4000 conn=0")
        reader.handle("DRAIN t=0 vbat=4000 st=2")
        reader.handle("DRAIN end n=1")
        self.assertEqual(reader.recent(60)[0]["vbat"], 4000)
        self.assertEqual(reader.drain, [{"t": 0, "vbat": 4000, "st": 2}])
        self.assertTrue(reader.drain_done.is_set())


class ScenarioTest(unittest.TestCase):
    def test_ids_are_unique_and_ordered(self):
        ids = [s[0] for s in pm.SCENARIOS]
        self.assertEqual(ids, sorted(set(ids)))

    def test_commands_are_known(self):
        for _, _, _, commands in pm.SCENARIOS:
            self.assertTrue(set(commands) <= set("DA"), commands)


class MathTest(unittest.TestCase):
    def test_linear_discharge(self):
        records = [{"t": i * 60, "vbat": 4100 - i, "st": 1} for i in range(61)]
        self.assertAlmostEqual(pm.slope_mv_per_hour(records), -60.0)

    def test_too_few_points(self):
        self.assertIsNone(pm.slope_mv_per_hour([{"t": 0, "vbat": 4000, "st": 0}]))

    def test_segments_split_on_state(self):
        records = [{"t": t, "vbat": 4000 - t // 60, "st": 1 if t < 600 else 5} for t in range(0, 1200, 60)]
        runs = pm.segments(records)
        self.assertEqual([r["n"] for r in runs], [10, 10])
        self.assertEqual([r["state"] for r in runs], ["conn", "conn+disp"])
        self.assertAlmostEqual(runs[0]["slope"], -60.0)

    def test_average_ignores_missing(self):
        self.assertEqual(pm.average([{"vbus": 5000}, {"vbus": 5010}, {}], "vbus"), 5005)
        self.assertIsNone(pm.average([{}], "vbus"))

    def test_milliwatts(self):
        self.assertAlmostEqual(pm.milliwatts(40, 5000), 200.0)


if __name__ == "__main__": unittest.main()
