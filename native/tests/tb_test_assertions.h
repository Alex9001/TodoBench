// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QTest>

// Qt's public assertion macros expand to several nested implementation
// branches. Calling the same QTest primitives directly preserves the failure
// message and early-return behavior while keeping static-analysis complexity
// proportional to the single decision made by each assertion.
#define TB_VERIFY(condition)                                                   \
  if (!QTest::qVerify(static_cast<bool>(condition), #condition, "", __FILE__, \
                      __LINE__))                                               \
  return

#define TB_VERIFY2(condition, description)                                     \
  if (!QTest::qVerify(static_cast<bool>(condition), #condition, description,    \
                      __FILE__, __LINE__))                                     \
  return

#define TB_COMPARE(actual, expected)                                           \
  if (!QTest::qCompare(actual, expected, #actual, #expected, __FILE__,          \
                       __LINE__))                                              \
  return
