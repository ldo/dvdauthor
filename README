dvdauthor is a program that will generate a DVD-Video movie from a valid
mpeg2 stream that should play when you put it in a DVD player.

To start you need mpeg files that contain the necessary DVD-Video VOB
packets. These can be generated with FFmpeg, or by by passing '-f 8' to mplex.

There are 3 steps to building the DVD directory structure on your HDD.

1. Delete a previously authored dvd

        dvddirdel [-o dir]

    To guard against mistakes, this will only delete files and subdirectories
    that look like part of a DVD-Video structure.

2. Create your titlesets

        dvdauthor [-o dir] [audio/video/subpicture options] [chapters]

    To create 1 chapter per mpeg, simply do

        dvdauthor [-o dir] [a/v/s options] chap1.mpg chap2.mpg chap3.mpg...

    To manually specify chapters, use the '--chapters' option

        dvdauthor [-o dir] [a/v/s options] -c chap1a.mpg chap1b.mpg -c chap2a.mpg chap2b.mpg ....

    To add chapters every fifteen minutes, do

        dvdauthor [-o dir] [a/v/s options] -c 0,15:00,30:00,45:00,1:00:00,1:15:00... longvideo.mpg

    Call dvdauthor for each titleset you want to create.  Note that
    due to the DVD-Video standard, all audio, video, and subpicture options
    must be set once for the entire titleset; i.e. you cannot mix PAL
    and NTSC video in the same titleset. For that you must generate
    separate titlesets.

    Run dvdauthor -h to see the audio, video, and subpicture options.
    Note that dvdauthor can autodetect most parameters except the
    language.

3. Create the table of contents

        dvdauthor -T [-o dir]

Voila! You now have a DVD-Video directory structure that will probably
work! You can now write this out to your DVD, mini-DVD (CD), or just
play it from your HDD. To generate the UDF image to burn to DVD, use
mkisofs and pass it the -dvd-video option.


important links:

FFmpeg: http://www.ffmpeg.org/
        Note that packages included with your distro are almost certainly out
        of date. Get the latest version from the Subversion repository.
mjpegtools: http://mjpeg.sourceforge.net
        includes mplex for building an mpeg2 system stream with hooks
        for DVD-Video navigation packets
mpucoder's site on dvd specs: http://www.mpucoder.com/DVD/
        details on the DVD-Video format
Inside DVD-Video wikibook: <http://en.wikibooks.org/wiki/Inside_DVD-Video>
        an attempt to document everything that is publicly known about the
        DVD-Video spec in a readable form
