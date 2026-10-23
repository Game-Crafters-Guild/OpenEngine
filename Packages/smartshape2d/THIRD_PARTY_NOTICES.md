# SmartShape2D

This native adaptation is based on the behavior and resource model of
[SmartShape2D](https://github.com/SirRamEsq/SmartShape2D), MIT licensed.
Reference revision: `9617190250d017529519ef3a6af279ea96a42efd`.
The upstream copyright and permission notice are preserved in [LICENSE](LICENSE).

The native package uses the engine's spline evaluator, polygon triangulator,
materials, scene serializer, editor tools, and physics backend. It does not
embed the upstream scripting runtime or load its project/resource file formats.

## Clipper2

Polygon inflation and open-line buffering use Clipper2 1.5.4, commit
`ef88ee97c0e759792e43a2b2d8072def6c9244e8`, from
https://github.com/AngusJohnson/Clipper2. Copyright Angus Johnson.
The vendored C++ sources in `Native/clipper2` are unmodified and distributed
under the Boost Software License 1.0, reproduced in that directory's `LICENSE`.
