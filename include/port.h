/* The platform port of SPEC.md Section 12 for the Nucleo-L476RG, and the node identity. */
#ifndef PORT_H
#define PORT_H

#include "lmesh/lmesh.h"

void port_init(lmesh_port *port);
void port_identity(lmesh_identity *ident);

#endif
