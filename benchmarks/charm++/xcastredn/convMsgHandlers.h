#ifndef CONV_MSG_HANDLERS_H
#define CONV_MSG_HANDLERS_H

/// Converse broadcast and reduction handler function IDs. Global vars.
extern int bcastHandlerID, rednHandlerID;

/// Converse msg handler. Converts a converse bcast to a charm array bcast
void convBcastHandler(void *env);
// Converse redn msg handler triggered at the root of the converse redn
void convRednHandler(void *env);
/// Converse redn merge fn triggered at each vertex along the redn tree
void* convRedn_sum (size_t *size, void *local, void **remote, int count);

// Registration function for all the converse msg handlers
void registerHandlers();

#endif // CONV_MSG_HANDLERS_H

