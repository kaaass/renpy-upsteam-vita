import renpy
import renpy.renpyvita as renpyvita
from renpy.audio.audio import MusicContext


class VitaVideoChannel(object):

    def __init__(self, name, file_prefix="", file_suffix="", default_loop=None):

        # A list of queued filenames.
        self.queue = [ ]

        # The filename that's currently playing.
        self.filename = None

        # The name assigned to this channel. This is used to look up
        # information about the channel in the MusicContext object.
        self.name = name

        # The name of the mixer this channel uses. Set below, as there's
        # no good default.
        self.mixer = None

        # The time the music in this channel was last changed.
        self.last_changed = 0

        # The callback that is called if the queue becomes empty.
        self.callback = None

        # Ignored.
        self.synchro_start = False
        self.wait_stop = False
        self.loop = [ ]

        # A prefix and suffix that are used to create the full filenames.
        self.file_prefix = file_prefix
        self.file_suffix = file_suffix

        if default_loop is None:
            # By default, should we loop the music?
            self.default_loop = True
            # Was this set explicitly?
            self.default_loop_set = False

        else:
            self.default_loop = default_loop
            self.default_loop_set = True

        # Init
        renpyvita.video_init()

    def get_context(self):
        """
        Returns the MusicContext corresponding to this channel, taken from
        the context object. Allocates a MusicContext if none exists.
        """

        mcd = renpy.game.context().music

        rv = mcd.get(self.name)
        if rv is None:
            rv = mcd[self.name] = MusicContext()

        return rv

    context = property(get_context)

    def copy_context(self):
        """
        Copies the MusicContext associated with this channel, updates the
        ExecutionContext to point to the copy, and returns the copy.
        """

        mcd = renpy.game.context().music

        ctx = self.get_context().copy()
        mcd[self.name] = ctx
        return ctx

    def start(self):
        """
        Starts playing the first video in the queue.
        """

        if not self.queue:
            return

        filename = self.queue.pop(0)

        with renpy.loader.load(filename) as f:
            real_filename = f.name

        print("Playing", filename, real_filename)

        renpyvita.video_start(real_filename)

    def stop(self):
        renpyvita.video_stop()
        self.filename = None

    def get_playing(self):
        return renpyvita.video_get_playing()

    def periodic(self):

        # This should be set from something that checks to see if our
        # mixer is muted.
        force_stop = self.context.force_stop

        if force_stop:
            self.dequeue()
            self.stop()
            return

        renpyvita.periodic()

        if self.get_playing():
            return

        if self.queue:
            self.start()

    def dequeue(self, even_tight=False):
        """
        Clears the queued music, except for a first item that has
        not been started.
        """

        if self.get_playing():
            self.queue = [ ]
        else:
            self.queue = self.queue[:1]

    def interact(self):
        """
        Called (mostly) once per interaction.
        """

        self.periodic()

    def fadeout(self, secs):
        """
        Causes the playing music to be faded out for the given number
        of seconds. Also clears any queued music.
        """

        self.stop()
        self.queue = [ ]

    def enqueue(self, filenames, loop=True, synchro_start=False, fadein=0, tight=None, loop_only=False, relative_volume=1.0):
        self.queue.extend(filenames)

    def set_volume(self, volume):
        pass

    def get_pos(self):
        pass

    def set_pan(self, pan, delay):
        pass

    def set_secondary_volume(self, volume, delay):
        pass

    def pause(self):
        # TODO
        pass

    def unpause(self):
        # TODO
        pass

    def reload(self):
        return

    def read_video(self):
        return renpyvita.video_read_video()

    def video_ready(self):
        return 1
