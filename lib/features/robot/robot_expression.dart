import '../voice/voice_session_controller.dart';

/// The 8 facial states the robot face renderer supports (Phase 5, Part D).
///
/// Only [neutral], [listening], [thinking], [speaking] and [offline] are
/// currently reachable from real application state — see
/// [mapConversationStateToExpression]. [asking], [happy] and [encouraging]
/// have no reliable signal to drive them yet (no per-question/per-answer
/// semantics anywhere in the app), so they're implemented in the renderer
/// and available on this enum for a later milestone, but nothing maps to
/// them today. Per this task's explicit constraint, that's deliberate: no
/// keyword heuristics on transcripts, no changes to the Gemini protocol
/// just to manufacture emotion signals.
enum RobotExpression {
  neutral,
  listening,
  thinking,
  asking,
  speaking,
  happy,
  encouraging,
  offline,
}

/// Maps the [VoiceSessionController]'s [ConversationState] — the only
/// reliable signal available — to a [RobotExpression]. Pure function so the
/// mapping is directly unit-testable without spinning up a real session.
///
/// [ConversationState.disconnected] and [ConversationState.error] both
/// resolve to [RobotExpression.neutral] rather than [RobotExpression
/// .offline] — by explicit request: the face used to visibly dim/change
/// look whenever the session dropped between uses (e.g. an idle Gemini
/// Live timeout between push-to-talk presses), which read as the robot
/// going into an unwanted "standby" look. [RobotExpression.offline] is
/// still implemented and still used for the one case that's genuinely
/// "nothing to do" rather than "not in use right now" — see
/// `RobotHomeScreen`'s own expression fallback for when there is no
/// enabled activity to run at all.
RobotExpression mapConversationStateToExpression(ConversationState state) {
  switch (state) {
    case ConversationState.disconnected:
    case ConversationState.error:
    case ConversationState.connecting:
    case ConversationState.ready:
      return RobotExpression.neutral;
    case ConversationState.listening:
      return RobotExpression.listening;
    case ConversationState.waiting:
      return RobotExpression.thinking;
    case ConversationState.speaking:
      return RobotExpression.speaking;
  }
}
