#pragma once
// Time limit of one protected call into a Luau script (no game access: tests/script_time_limit_checks.cpp).

// Called by the Luau interrupt during a protected call that started at `start` (0: no call running). Returns true when
// the call must stop with an error. Once the limit has passed, every later check in the same call returns true again
// (`expired` is cleared only when the next call starts): a script that catches the error with pcall or xpcall cannot
// keep running, the error is raised again until it reaches the outermost call.
inline bool ScriptOverLimit(double start,double limit,bool& expired,double now){
 if(start<=0)return false;
 if(now-start>limit)expired=true;
 return expired;}
