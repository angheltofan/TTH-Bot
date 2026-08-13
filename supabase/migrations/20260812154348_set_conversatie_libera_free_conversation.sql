-- The hands-free milestone (see VoiceSessionController.startFreeConversation)
-- makes free_conversation a genuinely implemented interaction mode. The
-- "Conversație liberă" row was seeded with 'push_to_talk' in the previous
-- migration (20260812124951_create_activities.sql), matching that phase's
-- scope ("push-to-talk stays on for now"). This corrects it — idempotent,
-- safe whether or not that migration's original 'push_to_talk' seed value
-- was already applied to this project.
update public.activities
set interaction_mode = 'free_conversation'
where title = 'Conversație liberă'
  and interaction_mode = 'push_to_talk';
