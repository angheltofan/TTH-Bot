import 'package:flutter_test/flutter_test.dart';
import 'package:supabase_flutter/supabase_flutter.dart';
import 'package:tth_bot/features/activities/activity_failure.dart';

PostgrestException _pg(String code) =>
    PostgrestException(message: 'server text', code: code);

void main() {
  test('classifies Supabase errors without looking at their text', () {
    expect(
      classifySupabaseError(_pg('42501'), hasSession: true),
      ActivityFailure.permissionDenied,
    );
    expect(
      classifySupabaseError(_pg('42501'), hasSession: false),
      ActivityFailure.sessionExpired,
    );
    expect(
      classifySupabaseError(_pg('PGRST116'), hasSession: true),
      ActivityFailure.notChanged,
    );
    for (final code in ['PGRST301', 'PGRST302', 'PGRST303', '401']) {
      expect(
        classifySupabaseError(_pg(code), hasSession: true),
        ActivityFailure.sessionExpired,
      );
    }
    expect(
      classifySupabaseError(const AuthException('x'), hasSession: true),
      ActivityFailure.sessionExpired,
    );
    expect(
      classifySupabaseError(_pg('23502'), hasSession: true),
      ActivityFailure.unavailable,
    );
    expect(
      classifySupabaseError(Exception('x'), hasSession: true),
      ActivityFailure.unavailable,
    );
  });

  test('unknown errors from any repository count as unavailable', () {
    expect(activityFailureOf(Exception('x')), ActivityFailure.unavailable);
    expect(
      activityFailureOf(
        const ActivityRepositoryException(ActivityFailure.notChanged),
      ),
      ActivityFailure.notChanged,
    );
  });

  test('every message is fixed text with the action prefix', () {
    for (final action in ActivityAction.values) {
      for (final failure in ActivityFailure.values) {
        final message = activityFailureMessage(failure, action);
        expect(message, isNotEmpty);
        expect(message, isNot(contains('Exception')));
        switch (action) {
          case ActivityAction.load:
            expect(message, startsWith('Nu am putut încărca activitățile'));
          case ActivityAction.save:
            expect(message, startsWith('Eroare la salvare'));
          case ActivityAction.delete:
            expect(message, startsWith('Eroare la ștergere'));
        }
      }
    }
  });

  test('the exception names only the failure kind', () {
    expect(
      const ActivityRepositoryException(
        ActivityFailure.permissionDenied,
      ).toString(),
      'ActivityRepositoryException(permissionDenied)',
    );
  });
}
