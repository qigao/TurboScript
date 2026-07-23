# Module Documentation

Domain-specific module guides for TurboScript.

---

## Finance Modules

### [Finance Module](FIN_MODULE.md)
Quantitative finance functions including portfolio optimization, factor analysis, and risk metrics.

**Key Functions**:
- Portfolio optimization (mean-variance, max Sharpe)
- Covariance matrix calculation
- Factor analysis (z-score, rank, neutralization)
- Risk metrics (volatility, drawdown, Sharpe ratio)

**Usage**:
```javascript
import("fin");

var returns = [0.01, 0.02, -0.01, 0.03];
var sharpe = strategy.sharpe(returns, 0.02, 252);
```

---

### [Portfolio Management](PORTFOLIO.md)
Advanced portfolio construction and optimization techniques.

**Topics**:
- Mean-variance optimization
- Risk parity
- Black-Litterman model
- Portfolio rebalancing

---

### [Factor Analysis](FACTORS.md)
Factor-based investment strategies and analysis.

**Topics**:
- Factor construction
- Factor neutralization
- Multi-factor models
- Factor backtesting

---

## Graph Modules

### [Graph Algorithms](GRAPH_ALGORITHMS.md)
Graph theory algorithms for network analysis.

**Algorithms**:
- Shortest path (Dijkstra, Bellman-Ford)
- Minimum spanning tree (Kruskal, Prim)
- Network flow
- Community detection

---

### [Graph Implementation](GRAPH_IMPLEMENTATION.md)
Internal implementation details of graph data structures.

**Topics**:
- Adjacency list representation
- Edge list representation
- Performance characteristics
- Memory layout

---

## Other Modules

### OS Module
See [OS module README](../../modules/os/README.md) for platform queries,
shell-free child process management, default logger access, service controls,
power controls, and in-process Cron schedules.

### Parser / Mapper / Datetime Modules
See [parser README](../../modules/parser/README.md) for configuration-oriented text parsing.
See [Mapper README](../../modules/mapper/README.md) for class-first JSON/YAML/XML
document mapping. See [csv_filter_expression.md](../csv_filter_expression.md)
for CSV filter expressions.

### RulesForge Module
See [rules_forge README](../../modules/rules_forge/README.md) for RulesForge knowledge-base/session handles, external RulesForge DataBind-backed fact insertion, query access, tracing, and TurboScript RHS plugin loading.

### SQLite / RAG Module
See [sqlite README](../../modules/sqlite/README.md) for SQLite SQL helpers, FTS5 probing, embedding BLOB storage, cosine search, and the `sqlite.rag_*` local RAG helpers.

### Technical Analysis
See [ta_fin_cheatsheet.md](../ta_fin_cheatsheet.md) for TA indicators.

### Vector Operations
See [vec_cheatsheet.md](../vec_cheatsheet.md) for advanced vector operations.

---

## Creating Custom Modules

See [Plugin Development Guide](../plugin-development.md) to learn how to create your own modules in C/C++.

---

**Module ecosystem for TurboScript**
