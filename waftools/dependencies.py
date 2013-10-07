from waflib.Configure import conf
from waflib.Logs import pprint

satisfied_deps = set()

class DependencyError(Exception):
    pass

class Dependency(object):
    def __init__(self, ctx, satisfied_deps, dependency):
        self.ctx = ctx
        self.satisfied_deps = satisfied_deps
        self.identifier, self.attributes = dependency['name'], dependency

    def check(self):
        self.ctx.start_msg('Checking for {0}'.format(self.attributes['desc']))

        try:
            self.check_disabled()
            self.check_dependencies()
            self.check_negative_dependencies()
            self.check_autodetect_func()
        except DependencyError:
            pass # This exception is used for control flow, don't mind it

    def check_disabled(self):
        if not self.enabled_option():
            self.skip()
            raise DependencyError

    def check_dependencies(self):
        if 'deps' in self.attributes:
            deps = set(self.attributes['deps'])
            if not deps <= self.satisfied_deps:
                missing_deps = deps - self.satisfied_deps
                self.fail("{0} not found".format(", ".join(missing_deps)))
                raise DependencyError

    def check_negative_dependencies(self):
        if 'deps_neg' in self.attributes:
            deps = set(self.attributes['deps_neg'])
            if deps <= self.satisfied_deps:
                conflicting_deps = deps & self.satisfied_deps
                self.skip("{0} found".format(", ".join(conflicting_deps)))
                raise DependencyError

    def check_autodetect_func(self):
        if self.attributes['func'](self.ctx, self.identifier):
            self.success(self.identifier)
        else:
            self.fail()


    def enabled_option(self):
        try:
            return getattr(self.ctx.options, self.enabled_option_repr())
        except AttributeError:
            pass
        return True

    def enabled_option_repr(self):
        return "enable_{0}".format(self.identifier)

    def success(self, depname):
        satisfied_deps.add(depname)
        self.ctx.end_msg('yes')

    def fail(self, reason='no'):
        self.ctx.end_msg(reason, 'RED')

    def skip(self, reason='disabled'):
        self.ctx.end_msg(reason, 'YELLOW')

def check_dependency(ctx, dependency):
    Dependency(ctx, satisfied_deps, dependency).check()

@conf
def detect_target_os_dependency(ctx):
    target = "os_{0}".format(ctx.env.DEST_OS)
    ctx.start_msg('Detected target OS:')
    ctx.end_msg(target)
    satisfied_deps.add(target)

@conf
def parse_dependencies(ctx, dependencies):
    [check_dependency(ctx, dependency) for dependency in dependencies]