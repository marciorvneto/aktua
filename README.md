# Aktua

**Aktua** is a small, experimental programming language for control systems and numerical computing, written in C.

It's a work in progress.

You can use it interactively through a REPL or execute scripts from the command line.

## Example

Level control of a leaky tank with a PI controller ([`examples/leaky_tank.akt`](examples/leaky_tank.akt)):

```rust
// Level control of a leaky tank with a PI controller.
// See tank_level_control.akt for where these numbers come from.
// By: Marcio R. V. Neto, 2026

let s = poly([0, 1])

let H = 0.2 / (100 * s + 1)    // Tank: level [m] per inflow [L/s], time constant 100 s
let C = 20 + 0.2 / s           // PI controller
let W = C * H / (1 + C * H)    // Closed loop: level setpoint -> level

print(W)
print(poles(W))

let t = linspace(0, 150, 151)
plot(t, step(W, t))
```

Output:

```text
     400
-------------
10000 x + 400
(
-0.04
)
1.0 ┤⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⣀⣀⣀⣠⠤⠤⠤⠤⠔⠒⠒⠒⠒⠒⠒⠒⠒⠒⠒⠊⠉⠉⠉⠉⠉⠉⠉⠉⠉⠉⠉⠉
    │⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⣀⡤⠤⠒⠒⠉⠉⠁⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
    │⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⡠⠖⠊⠁⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
0.8 ┤⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⡰⠊⠁⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
    │⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⡠⠜⠁⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
    │⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⠜⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
0.6 ┤⠀⠀⠀⠀⠀⠀⠀⠀⡸⠁⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
    │⠀⠀⠀⠀⠀⠀⢠⠊⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
0.4 ┤⠀⠀⠀⠀⠀⡠⠃⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
    │⠀⠀⠀⠀⢠⠃⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
    │⠀⠀⠀⡰⠁⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
0.2 ┤⠀⠀⢠⠃⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
    │⠀⢠⠃⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
    │⠀⡇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
0.0 ┤⡸⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
    └┬───────────────────┬──────────────────┬───────────────────┬
     0                  50                 100                 150
```

For the full story, with the tank modelled from first principles, P, PI and PID control compared and every result checked against theory, see [`examples/tank_level_control.akt`](examples/tank_level_control.akt).

## Quick start

Build the project and start the REPL:

```console
$ make
$ ./build/aktua
```

Try a few commands:

```text
aktua> let x = 3
aktua> x

3

aktua> let p = poly([1,2,3])
aktua> p

3 x^2 + 2 x + 1

aktua> 1/p

       1
---------------
3 x^2 + 2 x + 1
```

You can also execute scripts directly:

```console
$ ./build/aktua examples/leaky_tank.akt
```

## Vectors and matrices

Commas separate elements and semicolons separate rows:

```rust
let a = [1, 2, 3]            // a vector
let b = [1; 2; 3]            // a column, 3x1
let M = [1, 2, 3; 4, 5, 6]   // a matrix, 2x3
```

A few rules cover most of what you need:

- `+`, `-`, `*`, `/` and `^` work element by element. Math functions such as `sqrt` and `sin` do too.
- `@` is the matrix product: `M @ a` gives `[14, 32]`, and `a @ a` gives the dot product, `14`.
- `.T` transposes. A vector reads as a row, so `a.T` is a column, the same as `b`.
- `.shape` gives the sizes: `M.shape` is `[2, 3]`.
- Shapes that don't match are stretched where a size is 1 ([broadcasting](https://numpy.org/doc/stable/user/basics.broadcasting.html)). `M + a` adds `a` to every row, and `a.T * a` is the outer product, a 3x3 matrix.
- Functions that expect a vector, like `poly`, `plot` and `step`, accept rows and columns alike.

Polynomials and transfer functions can be evaluated on whole vectors, including complex ones. For example, the magnitude of a frequency response:

```text
aktua> let s = poly([0, 1])
aktua> let G = 1 / (s + 1)
aktua> let j = complex(0, 1)
aktua> abs(G(j * [0.1, 1, 10]))

[ 0.995037,  0.707107, 0.0995037]
```

Note that `*` is always element by element; use `@` for the matrix product. `.T` does not conjugate complex entries.

## Status

Aktua is under active development. Expect bugs, missing features, and breaking changes.

## Citing

If you use Aktua in your work, please cite it. GitHub's "Cite this repository" button, in the sidebar, gives the citation in APA and BibTeX formats, generated from [`CITATION.cff`](CITATION.cff). In BibTeX:

```bibtex
@software{viannaneto_aktua_2026,
  author = {Vianna Neto, M{\'a}rcio Ribeiro},
  title  = {Aktua: a small programming language for control systems},
  year   = {2026},
  url    = {https://github.com/marciorvneto/aktua}
}
```

## License

Aktua is licensed under the [MIT License](LICENSE.md).
