// web.h - browser front-end for the simulator
#pragma once
#include <functional>
#include <string>

struct WebSim {
  std::string log;                                 // console output shown in the page
  std::function<void()> boot;                      // (re)start the interpreter and the script
  std::function<void()> step;                      // run timers and touch events
  std::function<void(const std::string&)> runCode; // code typed into the page's prompt
};

void serveSim(WebSim& sim, int port);  // never returns
