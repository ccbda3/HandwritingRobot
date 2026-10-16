"""handwriting inspect 子命令测试。"""

from __future__ import annotations

import json
from pathlib import Path

from handwriting.cli import EXIT_ERROR, EXIT_OK, main

CHAR_PAYLOAD = {
    "format": "handwriting-capture/1.0",
    "meta": {
        "device": "数位板",
        "pressureSupported": True,
        "samplingHz": 150,
        "charCount": 2,
    },
    "samples": [
        {
            "index": 0,
            "char": "永",
            "rate": 150,
            "pressure": True,
            "strokes": [
                [[0.1, 0.1, 0.3, 0], [0.2, 0.2, 0.4, 8]],
                [[0.3, 0.3, 0.5, 0], [0.4, 0.4, 0.6, 8]],
            ],
        },
        {
            "index": 1,
            "char": "一",
            "rate": 148,
            "pressure": True,
            "strokes": [[[0.05, 0.5, 0.6, 0], [0.95, 0.5, 0.6, 60]]],
        },
    ],
}

LINE_PAYLOAD = {
    "format": "handwriting-capture/2.0",
    "meta": {"device": "数位板", "pressureSupported": True, "samplingHz": 150},
    "lines": [
        {
            "index": 0,
            "text": "一二三",
            "rate": 150,
            "pressure": True,
            "strokes": [
                [[0.00, 0.50, 0.5, 0], [0.10, 0.50, 0.5, 50]],
                [[0.20, 0.50, 0.5, 0], [0.30, 0.50, 0.5, 50]],
                [[0.40, 0.50, 0.5, 0], [0.50, 0.50, 0.5, 50]],
            ],
            "strokeStartT": [0, 200, 400],
            "charRanges": [[0, 1], [1, 2], [2, 3]],
        }
    ],
}


def _write(tmp_path: Path, payload: dict[str, object], name: str = "cap.json") -> Path:
    path = tmp_path / name
    path.write_text(json.dumps(payload), encoding="utf-8")
    return path


class TestInspect:
    def test_char_capture(self, tmp_path: Path, capsys) -> None:
        code = main(["inspect", str(_write(tmp_path, CHAR_PAYLOAD))])
        out = capsys.readouterr().out
        assert code == EXIT_OK
        assert "handwriting-capture/1.0" in out
        assert "样本数  ：2 字" in out
        assert "风格特征" in out
        assert "主倾角" in out

    def test_line_capture(self, tmp_path: Path, capsys) -> None:
        code = main(["inspect", str(_write(tmp_path, LINE_PAYLOAD))])
        out = capsys.readouterr().out
        assert code == EXIT_OK
        assert "handwriting-capture/2.0" in out
        assert "行数    ：1" in out
        assert "行级风格特征" in out
        assert "平均字距" in out

    def test_flags_missing_pressure(self, tmp_path: Path, capsys) -> None:
        payload = json.loads(json.dumps(CHAR_PAYLOAD))
        payload["meta"]["pressureSupported"] = False
        code = main(["inspect", str(_write(tmp_path, payload))])
        out = capsys.readouterr().out
        assert code == EXIT_ERROR
        assert "压感" in out

    def test_unknown_format(self, tmp_path: Path, capsys) -> None:
        code = main(["inspect", str(_write(tmp_path, {"format": "something/else"}))])
        out = capsys.readouterr().out
        assert code == EXIT_ERROR
        assert "无法识别的数据格式" in out

    def test_missing_file(self, tmp_path: Path, capsys) -> None:
        code = main(["inspect", str(tmp_path / "nope.json")])
        out = capsys.readouterr().out
        assert code == EXIT_ERROR
        assert "找不到文件" in out

    def test_bad_json(self, tmp_path: Path, capsys) -> None:
        path = tmp_path / "bad.json"
        path.write_text("{not json", encoding="utf-8")
        assert main(["inspect", str(path)]) == EXIT_ERROR
        assert "无法读取或解析" in capsys.readouterr().out
