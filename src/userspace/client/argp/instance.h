#ifndef SRC_USERSPACE_CLIENT_ARGP_INSTANCE_H_
#define SRC_USERSPACE_CLIENT_ARGP_INSTANCE_H_

int handle_instance_add(int argc, char **argv);
int handle_instance_remove(int argc, char **argv);

void print_instance_add_opts(char *prefix);
void print_instance_remove_opts(char *prefix);

#endif /* SRC_USERSPACE_CLIENT_ARGP_INSTANCE_H_ */
