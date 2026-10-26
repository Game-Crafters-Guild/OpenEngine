// The fixture of 10_comments/long_comment. The test names the line of every site.

// Five lines are not a site.
// Two.
// Three.
// Four.
// Five.

// Six lines are a site.
// Two.
// Three.
// Four.
// Five.
// Six.

/// A Doxygen block of five lines is not a site.
/// @param value The value.
/// @param scale The scale.
/// @return The result.
/// @note The trap.

/*
 * A block comment of six lines is a site; its empty line counts.

 *
 *
 */

int a = 0; // Comments after code
int b = 0; // are not full-line
int c = 0; // comments, so these
int d = 0; // six lines
int e = 0; // are not
int f = 0; // a run.

// Copyright (c) Example. A license notice is not a site.
// Two.
// Three.
// Four.
// Five.
// Six.

// SPDX-License-Identifier: MIT
// Two.
// Three.
// Four.
// Five.
// Six.

// Five lines, an empty line,
// and five lines
// are two runs.
// Four.
// Five.

// One.
// Two.
// Three.
// Four.
// Five.
int g = 0;
