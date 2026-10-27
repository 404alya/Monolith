-- GPU time per frame of each OpenGL "workload" segment of an nsys capture, by its position in
-- the frame. Usage: sqlite3 -header -column <capture>.sqlite < .claude/skills/benchmarking/nsys-segments.sql
--
-- A row of OPENGL_WORKLOAD is not one GL call: it runs from the call it is named after
-- (correlationId) to the next framebuffer call (endCorrelationId), so a row named after a draw
-- holds every draw up to the next glBindFramebuffer / glFramebufferTexture*. `calls` counts them.
-- Frames are cut at glXSwapBuffers (the headless bench swaps its hidden window too); `pos` is the
-- segment's index in its frame, so frames with the same pass sequence line up. The first and last
-- frames (load, shader compiles, warmup) are left out by `skip`.

create temp table A as
  select a.correlationId cid, s.value nm from OPENGL_API a join StringIds s on s.id = a.nameId;
create index A_cid on A(cid);
create temp table F as
  select cid, row_number() over (order by cid) fr from A where nm = 'glXSwapBuffers';
create temp table W as
  select w.correlationId c0, w.endCorrelationId c1, (w.end - w.start) / 1e6 ms, s.value nm
  from OPENGL_WORKLOAD w join StringIds s on s.id = w.nameId;
create temp table WF as
  select (select max(fr) from F where F.cid < W.c0) fr, W.* from W;
create temp table WP as
  select fr, row_number() over (partition by fr order by c0) pos, c1 - c0 calls, ms, nm from WF
  where fr > 20 and fr < (select max(fr) from F) - 5;

-- 1. GPU ms per frame by the call the segment is named after (what nsys's own summary shows)
select nm first_call, count(*) segments, round(sum(ms) / (select count(distinct fr) from WP), 3) ms_per_frame,
       round(avg(ms), 3) avg_ms, round(max(ms), 3) max_ms
from WP group by nm order by ms_per_frame desc limit 12;

-- 2. The segments of a frame in order (the most common segment count per frame only), with
--    the draws they hold: match `pos` against the pass order in R3D_End() (r3d_draw.c)
create temp table N as select fr, count(*) n from WP group by fr;
select pos, nm first_call, round(avg(calls), 1) calls, round(avg(ms), 3) avg_ms, round(max(ms), 3) max_ms
from WP where fr in (select fr from N where n = (select n from N group by n order by count(*) desc limit 1))
group by pos having avg_ms >= 0.02 order by pos;
