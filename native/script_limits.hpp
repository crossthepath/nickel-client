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

// Start and end of a protected call (ScriptCall). Begin returns the state before the call, to give back to End.
// A call made while another call of the same script is running (an API function that runs a script callback, such as
// useItem's done) keeps the running call's start and limit: it can neither reset nor extend them, and when it runs out
// of time the running call is out of time too. Only the outermost call starts and clears the clock.
struct ScriptCallState{double start=0;bool expired=false;};
inline ScriptCallState ScriptCallBegin(double& start,bool& expired,double now){
 const ScriptCallState outer{start,expired};
 if(start<=0){start=now;expired=false;}
 return outer;}
inline void ScriptCallEnd(double& start,bool& expired,const ScriptCallState& outer){
 if(outer.start<=0){start=0;expired=false;}
 else{start=outer.start;expired=outer.expired||expired;}}
