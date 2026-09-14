// The module has no entry point of its own (-sINVOKE_RUN=0); the host calls eng_init. Emscripten still
// wants a main symbol for an executable target.
int main() { return 0; }
