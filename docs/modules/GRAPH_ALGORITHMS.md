# Graph Algorithms Module

> **📊 Network analysis and shortest path algorithms for TurboScript**

---

## 📋 Overview

The `graph` module provides graph algorithms for network analysis, particularly useful for:

- 🔄 **Currency arbitrage detection** - Find profitable trading cycles
- 📊 **Shortest path computation** - Optimize routing and costs
- ⚠️ **Risk propagation analysis** - Model credit risk networks
- 🌐 **Dependency resolution** - Analyze relationships

---

## 🎯 Algorithms

### Bellman-Ford Algorithm

**Single-source shortest path** that handles negative edge weights.

**Time Complexity:** O(V × E)
**Space Complexity:** O(V)

**Advantages:**
- ✅ Handles negative weights
- ✅ Detects negative cycles
- ✅ Simple and robust

**Use when:**
- Graph has negative weights
- Need to detect negative cycles (arbitrage)
- Graph is sparse (E << V²)

---

## 📚 API Reference

### `graph.bellman_ford(edges, n_vertices, n_edges, source, dist, prev)`

Compute shortest paths from source to all vertices.

**Parameters:**
- `edges` - Edge list as flat vector `[from, to, weight, from, to, weight, ...]`
- `n_vertices` - Number of vertices (0 to n_vertices-1)
- `n_edges` - Number of edges
- `source` - Source vertex (0-indexed)
- `dist` - Output: distance vector (length = n_vertices)
- `prev` - Output: predecessor vector (length = n_vertices)

**Returns:**
- `0` - Success
- `-1` - Negative cycle detected

**Example:**
```javascript
// Graph: 0 -> 1 (5), 0 -> 2 (3), 1 -> 2 (1), 2 -> 1 (2)
let edges = [
    0, 1, 5,
    0, 2, 3,
    1, 2, 1,
    2, 1, 2
];

let dist = vec(3);
let prev = vec(3);

let result = graph.bellman_ford(edges, 3, 4, 0, dist, prev);

if (result == 0) {
    print("Shortest distances from vertex 0:");
    print("  To vertex 0:", dist[0]);  // 0
    print("  To vertex 1:", dist[1]);  // 5 (via 0->2->1)
    print("  To vertex 2:", dist[2]);  // 3 (via 0->2)
}
```

---

### `graph.has_negative_cycle(edges, n_vertices, n_edges)`

Check if graph contains a negative cycle.

**Parameters:**
- `edges` - Edge list `[from, to, weight, ...]`
- `n_vertices` - Number of vertices
- `n_edges` - Number of edges

**Returns:**
- `1` - Negative cycle exists
- `0` - No negative cycle

**Example:**
```javascript
// Graph with negative cycle: 0 -> 1 (1), 1 -> 2 (2), 2 -> 0 (-5)
let edges = [
    0, 1, 1,
    1, 2, 2,
    2, 0, -5
];

if (graph.has_negative_cycle(edges, 3, 3)) {
    print("⚠️ Negative cycle detected!");
}
```

---

### `graph.extract_path(prev, source, target, path)`

Extract path from predecessor array.

**Parameters:**
- `prev` - Predecessor array from `bellman_ford`
- `source` - Source vertex
- `target` - Target vertex
- `path` - Output: path vector (will be filled with vertex sequence)

**Returns:**
- Path length (number of vertices)
- `-1` if no path exists

**Example:**
```javascript
let edges = [0, 1, 5, 1, 2, 3, 0, 2, 10];
let dist = vec(3);
let prev = vec(3);

graph.bellman_ford(edges, 3, 3, 0, dist, prev);

let path = vec(10);
let path_len = graph.extract_path(prev, 0, 2, path);

print("Path from 0 to 2:");
for (let i = 0; i < path_len; i++) {
    print("  Vertex", path[i]);
}
// Output: Vertex 0, Vertex 1, Vertex 2
```

---

### `graph.detect_arbitrage(rates, n_currencies)`

Detect arbitrage opportunities in currency exchange rates.

**Parameters:**
- `rates` - Exchange rate matrix (n × n), where `rates[i*n+j]` = rate from currency i to j
- `n_currencies` - Number of currencies

**Returns:**
- `1` - Arbitrage opportunity exists
- `0` - No arbitrage

**Algorithm:** Converts rates to `-log(rate)` and checks for negative cycle.

**Example:**
```javascript
// 3 currencies: USD (0), EUR (1), GBP (2)
let rates = [
    1.0,  1.18, 1.38,  // USD -> USD, EUR, GBP
    0.85, 1.0,  1.17,  // EUR -> USD, EUR, GBP
    0.72, 0.86, 1.0    // GBP -> USD, EUR, GBP
];

if (graph.detect_arbitrage(rates, 3)) {
    print("💰 Arbitrage opportunity found!");
    print("Example: USD -> EUR -> GBP -> USD for profit");
}
```

**Why it works:**
- If you can multiply exchange rates in a cycle and get > 1, there's arbitrage
- Taking `-log(rate)` converts multiplication to addition
- Arbitrage cycle becomes negative cycle in the graph

---

## 🎯 Complete Examples

### Example 1: Shortest Path in Network

```javascript
// Network topology
//     5
//  0 ---> 1
//  |      |
//  |3     |1
//  v      v
//  2 ---> 3
//     2

let edges = [
    0, 1, 5,
    0, 2, 3,
    1, 3, 1,
    2, 3, 2
];

let n_vertices = 4;
let n_edges = 4;
let source = 0;

let dist = vec(n_vertices);
let prev = vec(n_vertices);

let result = graph.bellman_ford(edges, n_vertices, n_edges, source, dist, prev);

if (result == 0) {
    print("=== Shortest Paths from Vertex 0 ===");
    for (let i = 0; i < n_vertices; i++) {
        print("Distance to", i, ":", dist[i]);
    }

    // Extract path to vertex 3
    let path = vec(10);
    let path_len = graph.extract_path(prev, source, 3, path);

    print("\nPath to vertex 3:");
    for (let i = 0; i < path_len; i++) {
        if (i > 0) print(" ->", path[i]);
        else print(path[i]);
    }
}
```

**Output:**
```
=== Shortest Paths from Vertex 0 ===
Distance to 0: 0
Distance to 1: 5
Distance to 2: 3
Distance to 3: 5

Path to vertex 3:
0 -> 2 -> 3
```

---

### Example 2: Currency Arbitrage Detection

```javascript
// Real-world currency arbitrage detection
function check_arbitrage(rates, currencies) {
    let n = currencies.length;

    if (graph.detect_arbitrage(rates, n)) {
        print("🚨 ARBITRAGE ALERT!");

        // Find the cycle using Bellman-Ford
        let edges = vec(n * n * 3);
        let edge_idx = 0;

        for (let i = 0; i < n; i++) {
            for (let j = 0; j < n; j++) {
                if (i != j) {
                    edges[edge_idx++] = i;
                    edges[edge_idx++] = j;
                    edges[edge_idx++] = -log(rates[i * n + j]);
                }
            }
        }

        let dist = vec(n);
        let prev = vec(n);

        graph.bellman_ford(edges, n, edge_idx / 3, 0, dist, prev);

        print("Arbitrage cycle detected in currencies:");
        for (let i = 0; i < n; i++) {
            print(" ", currencies[i]);
        }

        return true;
    }

    return false;
}

// Example usage
let currencies = ["USD", "EUR", "GBP", "JPY"];
let rates = [
    1.0,   1.18,  1.38,  110.0,
    0.847, 1.0,   1.17,  93.2,
    0.725, 0.855, 1.0,   79.5,
    0.0091, 0.0107, 0.0126, 1.0
];

check_arbitrage(rates, currencies);
```

---

### Example 3: Network Cost Optimization

```javascript
// Find cheapest route through network
function find_cheapest_route(costs, n_nodes, start, end) {
    // Build edge list from cost matrix
    let edges = [];
    for (let i = 0; i < n_nodes; i++) {
        for (let j = 0; j < n_nodes; j++) {
            if (i != j && costs[i * n_nodes + j] > 0) {
                edges.push(i);
                edges.push(j);
                edges.push(costs[i * n_nodes + j]);
            }
        }
    }

    let n_edges = edges.length / 3;
    let dist = vec(n_nodes);
    let prev = vec(n_nodes);

    let result = graph.bellman_ford(edges, n_nodes, n_edges, start, dist, prev);

    if (result == -1) {
        print("Error: Negative cycle detected!");
        return null;
    }

    if (dist[end] == Infinity) {
        print("No route from", start, "to", end);
        return null;
    }

    // Extract path
    let path = vec(n_nodes);
    let path_len = graph.extract_path(prev, start, end, path);

    print("Cheapest route from", start, "to", end);
    print("Total cost:", dist[end]);
    print("Path:");
    for (let i = 0; i < path_len; i++) {
        if (i > 0) print(" ->", path[i]);
        else print(path[i]);
    }

    return {
        cost: dist[end],
        path: path,
        length: path_len
    };
}

// Example: Shipping network
let shipping_costs = [
    0,   10,  15,  20,
    10,  0,   35,  25,
    15,  35,  0,   30,
    20,  25,  30,  0
];

find_cheapest_route(shipping_costs, 4, 0, 3);
```

---

### Example 4: Risk Propagation Analysis

```javascript
// Model credit risk propagation through financial network
function analyze_risk_propagation(exposures, institutions, shock_source) {
    let n = institutions.length;

    // Convert exposures to negative log (higher exposure = shorter "distance")
    let edges = [];
    for (let i = 0; i < n; i++) {
        for (let j = 0; j < n; j++) {
            if (i != j && exposures[i * n + j] > 0) {
                edges.push(i);
                edges.push(j);
                edges.push(-log(exposures[i * n + j] + 1));
            }
        }
    }

    let n_edges = edges.length / 3;
    let dist = vec(n);
    let prev = vec(n);

    graph.bellman_ford(edges, n, n_edges, shock_source, dist, prev);

    print("=== Risk Propagation Analysis ===");
    print("Shock source:", institutions[shock_source]);
    print("\nRisk distances:");

    for (let i = 0; i < n; i++) {
        if (i != shock_source) {
            print(" ", institutions[i], ":", dist[i]);
        }
    }

    // Find most vulnerable institution
    let max_risk = -Infinity;
    let most_vulnerable = -1;

    for (let i = 0; i < n; i++) {
        if (i != shock_source && dist[i] < Infinity && dist[i] > max_risk) {
            max_risk = dist[i];
            most_vulnerable = i;
        }
    }

    if (most_vulnerable >= 0) {
        print("\nMost vulnerable:", institutions[most_vulnerable]);

        let path = vec(n);
        let path_len = graph.extract_path(prev, shock_source, most_vulnerable, path);

        print("Contagion path:");
        for (let i = 0; i < path_len; i++) {
            if (i > 0) print(" ->", institutions[path[i]]);
            else print(institutions[path[i]]);
        }
    }
}

// Example usage
let institutions = ["Bank A", "Bank B", "Bank C", "Bank D"];
let exposures = [
    0.0, 0.5, 0.3, 0.1,
    0.4, 0.0, 0.6, 0.2,
    0.2, 0.3, 0.0, 0.7,
    0.1, 0.2, 0.4, 0.0
];

analyze_risk_propagation(exposures, institutions, 0);
```

---

## 🎓 Algorithm Details

### Bellman-Ford Pseudocode

```
function BellmanFord(edges, n_vertices, source):
    // Initialize
    dist[source] = 0
    for v in vertices:
        if v != source:
            dist[v] = ∞
            prev[v] = null

    // Relax edges V-1 times
    for i = 1 to n_vertices - 1:
        for each edge (u, v, w) in edges:
            if dist[u] + w < dist[v]:
                dist[v] = dist[u] + w
                prev[v] = u

    // Check for negative cycles
    for each edge (u, v, w) in edges:
        if dist[u] + w < dist[v]:
            return ERROR  // Negative cycle exists

    return (dist, prev)
```

### Why It Works

**Relaxation:** If we can reach `v` through `u` with less cost, update the distance.

**V-1 iterations:** In a graph with V vertices, the longest simple path has V-1 edges. After V-1 iterations, all shortest paths are found.

**Negative cycle detection:** If we can still relax an edge after V-1 iterations, there's a negative cycle.

---

## ⚡ Performance Tips

### 1. Use Sparse Representation

```javascript
// ✅ GOOD: Only include existing edges
let edges = [
    0, 1, 5,
    0, 2, 3
];

// ❌ BAD: Include all possible edges with Infinity
let edges = [
    0, 1, 5,
    0, 2, 3,
    0, 3, Infinity,  // Wastes computation
    1, 2, Infinity,
    // ...
];
```

### 2. Early Termination

The implementation includes early termination: if no edges are relaxed in an iteration, the algorithm stops.

### 3. Batch Processing

```javascript
// Process multiple sources efficiently
function compute_all_pairs_shortest_paths(edges, n_vertices, n_edges) {
    let all_dist = [];

    for (let source = 0; source < n_vertices; source++) {
        let dist = vec(n_vertices);
        let prev = vec(n_vertices);

        graph.bellman_ford(edges, n_vertices, n_edges, source, dist, prev);
        all_dist.push(dist);
    }

    return all_dist;
}
```

---

## 🚨 Common Pitfalls

### ❌ Forgetting to Check Return Value

```javascript
// WRONG: Ignoring negative cycle
let dist = vec(n);
let prev = vec(n);
graph.bellman_ford(edges, n, m, 0, dist, prev);
// dist might be invalid if negative cycle exists!

// CORRECT: Check return value
let result = graph.bellman_ford(edges, n, m, 0, dist, prev);
if (result == -1) {
    print("Error: Negative cycle detected!");
    return;
}
```

### ❌ Wrong Edge Format

```javascript
// WRONG: Separate arrays
let from = [0, 1, 2];
let to = [1, 2, 0];
let weight = [5, 3, 2];

// CORRECT: Flat array
let edges = [
    0, 1, 5,
    1, 2, 3,
    2, 0, 2
];
```

### ❌ Insufficient Output Array Size

```javascript
// WRONG: Output arrays too small
let dist = vec(2);  // Only 2 elements
let prev = vec(2);
graph.bellman_ford(edges, 5, 10, 0, dist, prev);  // n_vertices = 5!

// CORRECT: Match n_vertices
let dist = vec(5);
let prev = vec(5);
```

---

## 📖 See Also

- **[FIN_MODULE.md](FIN_MODULE.md)** - Financial analysis functions
- **[FACTORS.md](FACTORS.md)** - Factor processing
- **[PORTFOLIO.md](PORTFOLIO.md)** - Portfolio optimization

---

## 📚 References

- Bellman, R. (1958). "On a routing problem"
- Ford, L. R.; Fulkerson, D. R. (1962). "Flows in Networks"
- Cormen et al. (2009). "Introduction to Algorithms" (3rd ed.)

---

**Built with ❤️ for network analysis and quantitative finance**
