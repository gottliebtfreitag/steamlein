# steamlein
a simple framework to express execution dependencies between modules and to execute them as parallelized as possible

Steamlein is the successor of the pipeline/ModuleChain concept that was implemented with the FUmanoids [fumanoids.de].

Steamlein makes great use of simplyfile's epoll to run modules only when they can be run.
