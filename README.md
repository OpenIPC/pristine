![OpenIPC logo][logo]

## Pristine
**_A powerful set of open source DirectShow filters for integrating IP cameras into Windows applications_**


### Overview

This project mainly allows for seamless integration with hardware-accelerated and software fallback video codecs on a well-supported but deprecated multimedia framework. Unfortunately, because of this, only H.264 will ever be supported out-of-the-box.

On the other hand, some third parties have come up with sets of filters that do just that, but they are either unnecessarily bulky, their licensing makes them tough to distribute or their integration is too quirky for rapid development.

In our case, we will be relying on readily available internal components whose royalties have already been paid off by the provider of the underlying piece of software or hardware fullfilling the job.


### Inner workings

Pristine takes an unprecedented approach to come around all the possible downsides the alternative solutions are facing by wrapping around Media Foundation "transforms" directly within DirectShow.

Depending on the requested input and output types at connection time, and depending on the needed specifications, the right decoder or encoder will be attached in a transparent manner.

Furthermore, appropriate COM interfaces will allow configuring advanced settings as available.


### Technical support and donations

Please **_[support our project](https://openipc.org/support-open-source)_** with donations or orders for development or maintenance. Thank you!


[logo]: https://openipc.org/assets/openipc-logo-black.svg