import 'package:flutter/material.dart';

import 'tth_colors.dart';

/// Light Tales & Tech Hub theme for the Flutter Web configuration app.
/// Deliberately plain: white/near-white surfaces, blue for standard
/// interactive elements, orange reserved for the primary creation CTA.
ThemeData buildTthWebTheme() {
  final colorScheme = ColorScheme.fromSeed(
    seedColor: TthColors.brandBlue,
    brightness: Brightness.light,
    primary: TthColors.brandBlue,
    secondary: TthColors.brandOrange,
    surface: TthColors.white,
  );

  return ThemeData(
    useMaterial3: true,
    colorScheme: colorScheme,
    scaffoldBackgroundColor: TthColors.webBackground,
    textTheme: Typography.blackMountainView.apply(
      bodyColor: TthColors.webTextDark,
      displayColor: TthColors.webTextDark,
    ),
    appBarTheme: const AppBarTheme(
      backgroundColor: TthColors.webBackground,
      foregroundColor: TthColors.webTextDark,
      elevation: 0,
      centerTitle: true,
    ),
    cardTheme: CardThemeData(
      color: TthColors.white,
      elevation: 0,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(16),
        side: const BorderSide(color: TthColors.webCardBorder),
      ),
    ),
    filledButtonTheme: FilledButtonThemeData(
      style: FilledButton.styleFrom(
        backgroundColor: TthColors.brandBlue,
        foregroundColor: TthColors.white,
        padding: const EdgeInsets.symmetric(horizontal: 24, vertical: 14),
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
      ),
    ),
    outlinedButtonTheme: OutlinedButtonThemeData(
      style: OutlinedButton.styleFrom(
        foregroundColor: TthColors.webTextDark,
        side: const BorderSide(color: TthColors.webCardBorder),
        padding: const EdgeInsets.symmetric(horizontal: 24, vertical: 14),
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
      ),
    ),
    inputDecorationTheme: InputDecorationTheme(
      filled: true,
      fillColor: TthColors.white,
      border: OutlineInputBorder(
        borderRadius: BorderRadius.circular(12),
        borderSide: const BorderSide(color: TthColors.webCardBorder),
      ),
      enabledBorder: OutlineInputBorder(
        borderRadius: BorderRadius.circular(12),
        borderSide: const BorderSide(color: TthColors.webCardBorder),
      ),
      focusedBorder: OutlineInputBorder(
        borderRadius: BorderRadius.circular(12),
        borderSide: const BorderSide(color: TthColors.brandBlue, width: 1.5),
      ),
    ),
  );
}
