#!/usr/bin/env python3
"""genVolume 工具单测（纯 stdlib unittest；pyopenvdb 仅 VdbReadTest 需要）。

覆盖：
  · JPVOL 读写往返 + 头字段（write_jpovvol / write_jpovvol1 / read_jpovvol）
  · 公共数组工具：downsample / apply_window / loop_half_xfade / npzseq.make_loop
  · `read_grid_dense` 的轴序 / 不截断 / 偏移 / 缺 grid（VDB，需 pyopenvdb）

历史根因（2026-10-15）：campfire VDB→jpovvol「躺着+乱码」= `read_grid_dense`
按 (nz,ny,nx) 分配数组，而 pyopenvdb `copyToArray` 约定首轴是 x（形状须 (nx,ny,nz)）
→ x/z 对调 + `nz>nx` 时静默截断 z。见 VdbReadTest。

运行：python3 -m unittest discover -s tools/jpov/genVolume -p 'test_*.py' -v
"""
import importlib.util
import os
import sys
import tempfile
import unittest

import numpy as np

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import jpvol   # noqa: E402  (需要在 sys.path 之后导入)


def _load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(_HERE, name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class JpvolFormatTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="genvol_" )

    def test_jpovvol2_roundtrip_u8(self):
        rng = np.random.default_rng(0)
        dens = rng.random((3, 5, 4, 6), dtype=np.float32)
        temp = rng.random((3, 5, 4, 6), dtype=np.float32)
        path = os.path.join(self.tmp, "a.jpovvol")
        X, Y, Z, F, nbytes = jpvol.write_jpovvol(path, dens, temp, dtype="u8")
        self.assertEqual((X, Y, Z, F), (6, 4, 5, 3))          # nx,ny,nz,nframes（[F][z][y][x]）
        self.assertEqual(nbytes, 40 + 3 * 5 * 4 * 6 * 2)
        back = jpvol.read_jpovvol(path)
        self.assertEqual(back.shape, (3, 5, 4, 6, 2))
        # u8 量化误差 ≤ 0.5/255
        np.testing.assert_allclose(back[..., 0], dens, atol=1.0 / 255.0 + 1e-6)
        np.testing.assert_allclose(back[..., 1], temp, atol=1.0 / 255.0 + 1e-6)

    def test_jpovvol2_roundtrip_f32_exact(self):
        dens = np.linspace(0, 1, 2 * 3 * 3 * 3, dtype=np.float32).reshape(2, 3, 3, 3)
        temp = 1.0 - dens
        path = os.path.join(self.tmp, "b.jpovvol")
        jpvol.write_jpovvol(path, dens, temp, dtype="f32")
        back = jpvol.read_jpovvol(path)
        np.testing.assert_array_equal(back[..., 0], dens)     # f32 应逐字节一致
        np.testing.assert_array_equal(back[..., 1], temp)

    def test_jpovvol1_roundtrip(self):
        dens = np.arange(2 * 3 * 4, dtype=np.float32).reshape(2, 3, 4) / 24.0
        temp = dens * 0.5
        path = os.path.join(self.tmp, "c.jpovvol")
        X, Y, Z, nbytes = jpvol.write_jpovvol1(path, dens, temp)
        self.assertEqual((X, Y, Z), (4, 3, 2))
        self.assertEqual(nbytes, 32 + 2 * 3 * 4 * 2 * 4)
        back = jpvol.read_jpovvol(path)
        self.assertEqual(back.shape, (1, 2, 3, 4, 2))
        np.testing.assert_array_equal(back[0, ..., 0], dens)
        np.testing.assert_array_equal(back[0, ..., 1], temp)

    def test_read_rejects_garbage(self):
        path = os.path.join(self.tmp, "bad.jpovvol")
        with open(path, "wb") as f:
            f.write(b"NOTAVOL\x00" + b"\x00" * 32)
        with self.assertRaises(ValueError):
            jpvol.read_jpovvol(path)


class JpvolOpsTest(unittest.TestCase):
    def test_downsample_block_mean(self):
        a = np.arange(4 * 4 * 4, dtype=np.float32).reshape(1, 4, 4, 4)
        d = jpvol.downsample(a, 2)
        self.assertEqual(d.shape, (1, 2, 2, 2))
        # 每个输出 = 对应 2×2×2 块均值
        self.assertAlmostEqual(float(d[0, 0, 0, 0]), a[0, 0:2, 0:2, 0:2].mean())
        self.assertAlmostEqual(float(d[0, 1, 1, 1]), a[0, 2:4, 2:4, 2:4].mean())
        # s<=1 原样
        self.assertIs(jpvol.downsample(a, 1), a)

    def test_apply_window_edges(self):
        # 底面(y=0)不淡；顶面淡到 0；x/z 两端淡到 0；中心保持 1。
        dens = np.ones((1, 8, 8, 8), dtype=np.float32)
        temp = np.ones((1, 8, 8, 8), dtype=np.float32)
        dw, tw = jpvol.apply_window(dens, temp, 0.25)
        self.assertAlmostEqual(float(dw[0, 0, 7, 0]), 0.0, places=6)   # y 顶淡出
        self.assertAlmostEqual(float(dw[0, 0, 0, 0]), 0.0, places=6)   # x 端淡出
        self.assertAlmostEqual(float(dw[0, 7, 0, 0]), 0.0, places=6)   # z 端（z 最大）也淡出
        self.assertAlmostEqual(float(dw[0, 0, 0, 7]), 0.0, places=6)   # x 另一端
        self.assertAlmostEqual(float(dw[0, 4, 3, 4]), 1.0, places=6)   # 内部
        # 底面 y=0 行不淡（火坐地）
        self.assertAlmostEqual(float(dw[0, 4, 0, 4]), 1.0, places=6)
        # f<=0 = 不改
        d2, t2 = jpvol.apply_window(dens, temp, 0.0)
        self.assertIs(d2, dens)

    def test_loop_half_xfade_constant_and_seam(self):
        c = np.full((8, 4, 4, 4), 0.3, dtype=np.float32)
        out = jpvol.loop_half_xfade(c)
        np.testing.assert_allclose(out, c, atol=1e-6)               # 常量不变
        a = np.arange(8, dtype=np.float32).reshape(8, 1, 1, 1)
        out = jpvol.loop_half_xfade(a, 0.5)
        off = 8 // 2
        np.testing.assert_allclose(out[0], a[off], atol=1e-6)       # c[0]=1 ⇒ L[0]=a[off]
        self.assertFalse(np.allclose(out, a))                       # 非恒真：确实改了

    def test_npzseq_make_loop_moves_tail_toward_head(self):
        npzseq = _load("npzseq_to_jpovvol")
        dens = np.zeros((10, 2, 2, 2), dtype=np.float32)
        dens[0:3] = 1.0                      # 开头 b=3 帧 = 1（尾部向它们淡化）
        temp = dens.copy()
        o, _ = npzseq.make_loop(dens.copy(), temp.copy(), 3)
        self.assertGreater(float(o[-1, 0, 0, 0]), 0.0)              # 尾帧被拉向开头
        self.assertLess(float(o[-1, 0, 0, 0]), 1.0)                 # 但未完全替换
        self.assertAlmostEqual(float(o[0, 0, 0, 0]), 1.0, places=6)  # 头不变


class VdbReadTest(unittest.TestCase):
    """read_grid_dense 的轴序 / 截断（需 pyopenvdb）。"""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="genvol_vdb_")
        self.path = os.path.join(self.tmp, "synthetic.vdb")
        self.conv = _load("vdbseq_to_jpovvol")

    def _write_grid(self, name, lo, dims, fn):
        import pyopenvdb as vdb
        g = vdb.FloatGrid()
        g.name = name
        g.background = 0.0
        acc = g.getAccessor()
        nx, ny, nz = dims
        for iz in range(nz):
            for iy in range(ny):
                for ix in range(nx):
                    acc.setValueOn((lo[0] + ix, lo[1] + iy, lo[2] + iz),
                                   float(fn(ix, iy, iz)))
        vdb.write(self.path, [g])

    def test_axis_order_and_no_truncation(self):
        dims = (5, 7, 11)   # nx,ny,nz；nz>nx → 旧代码会丢 z>=nx 的体素
        self._write_grid("density", (0, 0, 0), dims, lambda x, y, z: 1.0 + x + 10 * y + 100 * z)
        arr = self.conv.read_grid_dense(self.path, "density", np.array([0, 0, 0]), np.array(dims))
        nx, ny, nz = dims
        self.assertEqual(arr.shape, (nz, ny, nx))                   # [z][y][x]
        self.assertGreater(float(arr.sum()), 0.0)
        expected = np.empty((nz, ny, nx), dtype=np.float32)
        for z in range(nz):
            for y in range(ny):
                for x in range(nx):
                    expected[z, y, x] = 1.0 + x + 10 * y + 100 * z
        np.testing.assert_allclose(arr, expected, atol=1e-6)
        self.assertAlmostEqual(float(arr[nz - 1, 0, 0]), 1.0 + 100 * (nz - 1), places=5)

    def test_placed_into_common_box_with_offset(self):
        self._write_grid("density", (3, 4, 5), (4, 5, 6), lambda x, y, z: 1.0 + x + 10 * y + 100 * z)
        arr = self.conv.read_grid_dense(self.path, "density",
                                        np.array([0, 0, 0]), np.array([8, 9, 12]))
        self.assertEqual(arr.shape, (12, 9, 8))
        self.assertAlmostEqual(float(arr[5, 4, 3]), 1.0, places=5)  # 体素(3,4,5)
        self.assertAlmostEqual(float(arr[10, 8, 6]), 1.0 + 3 + 40 + 500, places=5)
        self.assertEqual(float(arr[0, 0, 0]), 0.0)

    def test_missing_grid_is_zeros(self):
        self._write_grid("density", (0, 0, 0), (3, 3, 3), lambda x, y, z: 1.0)
        arr = self.conv.read_grid_dense(self.path, "flames", np.array([0, 0, 0]), np.array([3, 3, 3]))
        self.assertEqual(arr.shape, (3, 3, 3))
        self.assertEqual(float(arr.sum()), 0.0)

    def test_old_buggy_layout_would_fail(self):
        """非恒真护栏：旧 (nz,ny,nx) 布局在同一夹具上必须给出**不同**结果。"""
        import pyopenvdb as vdb
        dims = (5, 7, 11)
        self._write_grid("density", (0, 0, 0), dims, lambda x, y, z: 1.0 + x + 10 * y + 100 * z)
        correct = self.conv.read_grid_dense(self.path, "density", np.array([0, 0, 0]), np.array(dims))
        g = vdb.read(self.path, "density")
        b = g.evalActiveVoxelBoundingBox()
        gs = np.array(b[1]) - np.array(b[0]) + 1
        buggy = np.zeros((int(gs[2]), int(gs[1]), int(gs[0])), dtype=np.float32)   # OLD shape
        g.copyToArray(buggy, (0, 0, 0))
        self.assertFalse(np.allclose(buggy, correct), "旧布局竟与正确结果一致 → 断言失去意义")


if __name__ == "__main__":
    unittest.main(verbosity=2)
