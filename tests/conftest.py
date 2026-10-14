"""pytest 全局配置。

联网测试默认跳过：CI 与日常开发都不该依赖外部网络，
字形数据的真实下载由 ``--run-network`` 显式启用。
"""

from __future__ import annotations

import pytest


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption(
        "--run-network",
        action="store_true",
        default=False,
        help="运行需要联网的测试（字形数据下载）",
    )


def pytest_collection_modifyitems(config: pytest.Config, items: list[pytest.Item]) -> None:
    if config.getoption("--run-network"):
        return
    skip_network = pytest.mark.skip(reason="需要联网，用 --run-network 启用")
    for item in items:
        if "network" in item.keywords:
            item.add_marker(skip_network)
