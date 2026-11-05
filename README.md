# Toadi_dev

Work-in-progress notes, media and published builds for the [Toadi / EEVE]
robot mower.

## Contents

- [`Toadi-eyes-animation/`](Toadi-eyes-animation/) — **eyes animation plug-in**:
  documentation, source code, the accepted binary, a setup howto and a
  one-command deployment script. Start at
  [`Toadi-eyes-animation/README.md`](Toadi-eyes-animation/README.md) or go
  straight to [`SETUP.md`](Toadi-eyes-animation/SETUP.md).

## Safety

The Toadi is a physical robot mower. The plug-in itself only draws on the eye
panels and runs a REST listener — it does not move the robot — but anything
that touches the wider stack (restarts, takeovers, motion commands) must be
done with the physical e-stop within reach. See the safety sections in the
published documentation.

[Toadi / EEVE]: https://toadi.be/
