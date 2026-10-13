// ViewController.h
#import <Cocoa/Cocoa.h>
#import <SpriteKit/SpriteKit.h>
#import <GameplayKit/GameplayKit.h>
#import <AudioToolbox/AudioToolbox.h>


@interface ViewController : NSViewController

@property (nonatomic, strong) CALayer *emulationLayer;
@property (nonatomic, strong) NSTimer *emulationTimer;

- (void)startEmulation;
- (void)stopEmulation;
- (void)initAudioEngine;
- (void)setupMenu;

@end
