-- 16 — Self-reported workout exercise logs
-- Players log weight, sets, reps for exercises marked as 'self_report' in the template.

CREATE TABLE public.workout_exercise_logs (
  id              uuid PRIMARY KEY DEFAULT extensions.uuid_generate_v4(),
  assignment_id   uuid NOT NULL REFERENCES public.workout_assignments(id) ON DELETE CASCADE,
  player_id       uuid NOT NULL REFERENCES public.players(id) ON DELETE CASCADE,
  exercise_name   text NOT NULL,
  weight_lbs      numeric,
  sets_completed  int NOT NULL DEFAULT 0,
  reps_per_set    int,
  notes           text,
  logged_at       timestamptz NOT NULL DEFAULT now(),
  created_at      timestamptz NOT NULL DEFAULT now(),

  UNIQUE (assignment_id, player_id, exercise_name)
);

CREATE INDEX idx_wel_assignment_id ON public.workout_exercise_logs(assignment_id);
CREATE INDEX idx_wel_player_id ON public.workout_exercise_logs(player_id);

ALTER TABLE public.workout_exercise_logs ENABLE ROW LEVEL SECURITY;

-- Coach can view/manage logs on their teams
CREATE POLICY "Coaches can CRUD logs on own teams"
  ON public.workout_exercise_logs FOR ALL
  USING (
    EXISTS (
      SELECT 1 FROM public.workout_assignments wa
      JOIN public.teams t ON t.id = wa.team_id
      WHERE wa.id = workout_exercise_logs.assignment_id
        AND t.coach_id = auth.uid()
    )
  );

-- Players can view/insert their own logs
CREATE POLICY "Players can CRUD own logs"
  ON public.workout_exercise_logs FOR ALL
  USING (
    player_id IN (
      SELECT p.id FROM public.players p
      WHERE p.linked_user_id = auth.uid()
    )
  );
