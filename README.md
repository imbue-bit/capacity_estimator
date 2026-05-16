# capacity_estimator

本项目用于量化交易策略的容量测算。

通过 Q-learning 推理友好的在 Level-2 盘口数据上训练状态依赖的滑点乘数，将静态市场冲击转化为非线性的动态成本，从而更精准地估算策略的 AUM 上限。

### 1. Orderbook Discretizer
提取 L2 Tick 特征，将连续市场状态离散化为 MDP 有限状态空间。

| 状态维度 | Threshold | Categories | 说明 |
| --- | --- | --- | --- |
| Spread | 基于设定的 `spread_th` | narrow (窄) / wide (宽) | 反映买卖价差的宽窄程度 |
| Imbalance | 基于设定的 `imb_th` | ask_heavy (压仓) / balanced (平衡) / bid_heavy (托仓) | 反映盘口挂单的失衡状态 |

### 2. Q-Learning Oracle
通过离线遍历历史 L2 数据，模拟不同参与率 (Participation Rate: 1%, 5%, 10%) 下的执行成本，并更新 Q-Table：

| 项 | 描述 | 定义与计算公式 | 含义 |
| --- | --- | --- | --- |
| Actions | 参与率控制 | 3 种离散参与率 | 决定当前执行算法时的下单强度或执行速度。 |
| Reward | 总奖励 | Reward = - (Instant Cost + Delay Penalty) | 以最小化综合成本为目标（成本越低，奖励越高）。 |
| | ↳ *Instant Cost*<br>*(即时成本)* | 即时跨越买卖价差的成本 + 基础冲击成本 | 衡量为了保证即时成交（吃单/Market Order）所付出的直接流动性成本。 |
| | ↳ *Delay Penalty*<br>*(延迟惩罚)* | 未成交头寸在 `lookahead_ticks` 期间承受的不利价格漂移 | 衡量挂单未即时成交时，由于市场价格朝不利方向运动带来的逆向选择 (Adverse Selection) 风险。 |
| Output (输出) | 冲击乘数 | 训练收敛后，通过 Q-Table 中各状态的极值推导 | 最终输出该盘口状态下的动态冲击乘数，用于指导后续的量化交易决策。 |

### 3. Capacity Estimator

基于给定的交易信号和已训练的 Q-Learning Oracle，在指定 AUM 范围内进行 Grid search。流程如下：

1. 提取信号时刻对应的 L2 盘口特征。
2. 结合理论 Alpha、基础常数系数、AUM 对应的订单规模，以及 Oracle 输出的盘口动态惩罚系数，计算单笔冲击成本。
3. 对超额参与率 (超出 max_participation_rate) 施加二次流动性惩罚。
4. 汇总总净收益 (Net PnL)，寻优输出使得净收益最大化的最佳策略容量。

## 编译指南

系统环境要求：
* 支持 C++17 标准的编译器 (GCC 7+ 或 Clang 5+)
* GNU Make

```bash
# 编译可执行文件，输出至 bin/capacity_estimator
make all

# 清理构建产物
make clean
```

## 数据输入格式

输入数据需为严格格式化的 CSV 文件。解析器采用轻量级指针移动解析，无视表头（默认跳过第一行），且不允许存在缺失值或非数值类型。

### L2 Ticks

字段顺序严格要求如下：
timestamp, last_price, bid1_p, bid1_v, ask1_p, ask1_v, volume, turnover

### Trades

字段顺序严格要求如下：
timestamp, direction, target_weight, alpha_bps

* direction: 1 (Buy) 或 -1 (Sell)
* target_weight: 目标仓位权重 (double)
* alpha_bps: 该笔交易的预期超额收益基点 (double)

## 运行参数与使用示例

### Showcase

```bash
./bin/capacity_estimator --l2 /path/to/l2.csv --trades /path/to/trades.csv [options]
```


### Options

| 参数 | 类型 | 默认值 | 描述 |
| --- | --- | --- | --- |
| --vol | float | 0.015 | 标的日波动率 |
| --adv | float | 1e10 | 标的日均成交额 |
| --spread-th | float | 0.5 | 价差宽窄判定的阈值 (bps) |
| --imb-th | float | 0.33 | 盘口不平衡判定阈值 |
| --lr | float | 0.1 | Q-Learning 初始学习率 |
| --eps | float | 0.2 | Q-Learning 探索率 |
| --episodes | int | 10 | 强化学习训练轮数 |
| --lookahead | int | 10 | 计算价格漂移的前瞻 Tick 数 |
| --aum-max | float | 1e10 | 测算的最大 AUM 规模上限 |
| --aum-step | float | 1e7 | 容量测算的步进间隔 |
| --base-impact | float | 0.15 | 基础市场冲击系数常数 |

### Example

```bash
./bin/capacity_estimator \
  --l2 data/2026_l2_ticks.csv \
  --trades data/strategy_signals.csv \
  --episodes 20 \
  --lookahead 15 \
  --aum-max 5000000000 \
  --aum-step 50000000
```
