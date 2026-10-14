// ViewController.m
#import "ViewController.h"
#import "simulator_orion128.h"
#import <Carbon/Carbon.h>
#import <AudioToolbox/AudioToolbox.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_BUFFER_SIZE 1024

extern bool rk_cc_pressed;                  // Модификатор CC (Shift) -> PC5
extern bool rk_us_pressed;                  // Модификатор УС (Ctrl) -> PC6
extern bool rk_rus_lat_pressed;

// Время удержания РУС/ЛАТ: монитор опрашивает клавиатуру раз в несколько кадров
#define RUS_LAT_HOLD_SECONDS 0.1

// stringWithUTF8String: возвращает nil на невалидном UTF-8 (например, имя файла ORDOS в КОИ-8),
// а NSAlert не принимает nil. Битые байты заменяем на '?'.
static NSString *SafeUTF8String(const char *msg) {
    NSString *s = [NSString stringWithUTF8String:msg];
    if (s) return s;
    size_t n = strlen(msg);
    NSMutableData *out = [NSMutableData dataWithCapacity:n];
    for (size_t i = 0; i < n; ) {
        unsigned char c = (unsigned char)msg[i];
        size_t len = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
        BOOL ok = len > 0 && i + len <= n;
        for (size_t k = 1; ok && k < len; k++) ok = (((unsigned char)msg[i + k]) & 0xC0) == 0x80;
        if (ok) { [out appendBytes:msg + i length:len]; i += len; }
        else    { [out appendBytes:"?" length:1]; i++; }
    }
    s = [[NSString alloc] initWithData:out encoding:NSUTF8StringEncoding];
    return s ?: @"(сообщение недоступно)";
}

@interface ViewController () {
    AudioQueueRef audioQueue;
    AudioQueueBufferRef audioBuffers[2];
    NSUInteger _rusLatToken;   // поколение нажатия РУС/ЛАТ: устаревшие отложенные отпускания игнорируются
    BOOL _lastCapsLock;        // предыдущее состояние Caps Lock
}
@end

@implementation ViewController

static void AudioQueueCallback(void *inUserData, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer) {
    int sampleCount = inBuffer->mAudioDataBytesCapacity / sizeof(int16_t);
    orion128_get_audio_samples((int16_t *)inBuffer->mAudioData, sampleCount);
    inBuffer->mAudioDataByteSize = inBuffer->mAudioDataBytesCapacity;
    AudioQueueEnqueueBuffer(inAQ, inBuffer, 0, NULL);
}

- (void)viewDidLoad {
    [super viewDidLoad];
    
    _lastCapsLock = ([NSEvent modifierFlags] & NSEventModifierFlagCapsLock) != 0;
    
    // Инициализация слоя рендеринга CoreAnimation
    self.view.wantsLayer = YES;
    self.emulationLayer = [CALayer layer];
    self.emulationLayer.frame = self.view.bounds;
    self.emulationLayer.contentsGravity = kCAGravityResizeAspect;
    
    // Pixel-art фильтрация
    self.emulationLayer.magnificationFilter = kCAFilterNearest;
    [self.view.layer addSublayer:self.emulationLayer];
    
    // Поиск файла прошивки Orion ROM BIOS
    NSString *monitorPath = [[NSBundle mainBundle] pathForResource:@"monitor" ofType:@"rom"];
    if (!monitorPath) {
        NSAlert *alert = [[NSAlert alloc] init];
        [alert setMessageText:@"Ошибка ресурсов"];
        [alert setInformativeText:@"Файл 'monitor.rom' не найден в Bundle приложения.\nДобавьте образ BIOS Монитора в Xcode."];
        [alert setAlertStyle:NSAlertStyleCritical];
        [alert runModal];
        
        dispatch_async(dispatch_get_main_queue(), ^{
            [NSApp terminate:nil];
        });
        return;
    }
    // Поиск файла прошивки Orion ROMDISK
    NSString *romdiskPath = [[NSBundle mainBundle] pathForResource:@"romdisk" ofType:@"rom"];
    if (!romdiskPath) {
        NSAlert *alert = [[NSAlert alloc] init];
        [alert setMessageText:@"Ошибка ресурсов"];
        [alert setInformativeText:@"Файл 'romdisk.rom' не найден в Bundle приложения.\nДобавьте образ BIOS Монитора в Xcode."];
        [alert setAlertStyle:NSAlertStyleCritical];
        [alert runModal];
        
        dispatch_async(dispatch_get_main_queue(), ^{
            [NSApp terminate:nil];
        });
        return;
    }
    
    orion128_init([monitorPath UTF8String], [romdiskPath UTF8String]);
    [self initAudioEngine];
    [self startEmulation];
}

- (void)initAudioEngine {
    AudioStreamBasicDescription format = {0};

    format.mSampleRate = AUDIO_SAMPLE_RATE;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags =
        kLinearPCMFormatFlagIsSignedInteger |
        kLinearPCMFormatFlagIsPacked;
    format.mBytesPerPacket = sizeof(int16_t);
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(int16_t);
    format.mChannelsPerFrame = 1;
    format.mBitsPerChannel = 16;

    OSStatus status = AudioQueueNewOutput(
        &format,
        AudioQueueCallback,
        (__bridge void *)(self),
        NULL,
        NULL,
        0,
        &audioQueue
    );

    if (status != noErr || !audioQueue) {
        NSLog(@"AudioQueueNewOutput failed: %d", (int)status);
        return;
    }

    for (int i = 0; i < 2; i++) {
        status = AudioQueueAllocateBuffer(
            audioQueue,
            AUDIO_BUFFER_SIZE * sizeof(int16_t),
            &audioBuffers[i]
        );

        if (status != noErr) {
            NSLog(@"AudioQueueAllocateBuffer failed: %d",
                  (int)status);

            AudioQueueDispose(audioQueue, true);
            audioQueue = NULL;

            for (int j = 0; j < i; j++) {
                audioBuffers[j] = NULL;
            }

            return;
        }

        AudioQueueCallback(
            (__bridge void *)(self),
            audioQueue,
            audioBuffers[i]
        );
    }

    status = AudioQueueStart(audioQueue, NULL);

    if (status != noErr) {
        NSLog(@"AudioQueueStart failed: %d", (int)status);
        AudioQueueDispose(audioQueue, true);
        audioQueue = NULL;

        audioBuffers[0] = NULL;
        audioBuffers[1] = NULL;
    }
}

- (void)dealloc {
    [self stopEmulation];

    if (audioQueue) {
        AudioQueueStop(audioQueue, true);
        AudioQueueDispose(audioQueue, true);
        audioQueue = NULL;
    }
}

- (void)viewDidLayout {
    [super viewDidLayout];
    self.emulationLayer.frame = self.view.bounds;
}

- (void)startEmulation {
    [self stopEmulation];
    // Интервал 0.02 секунды соответствует кадровой частоте развертки 50 Гц
    // Блочный таймер со слабой ссылкой: NSTimer с target:self удерживал бы контроллер,
    // и dealloc (а с ним остановка таймера и аудио) никогда бы не вызвался
    __weak typeof(self) weakSelf = self;
    self.emulationTimer = [NSTimer timerWithTimeInterval:0.02 repeats:YES block:^(NSTimer *timer) {
        ViewController *strongSelf = weakSelf;
        if (!strongSelf) {
            [timer invalidate];
            return;
        }
        [strongSelf emulationLoop];
    }];
    [[NSRunLoop currentRunLoop] addTimer:self.emulationTimer forMode:NSRunLoopCommonModes];
}

- (void)stopEmulation {
    if (self.emulationTimer) {
        [self.emulationTimer invalidate];
        self.emulationTimer = nil;
    }
}

- (void)emulationLoop {
    orion128_run_frame();
    const uint32_t *buffer = orion128_get_pixel_buffer();
    [self updateViewWithBuffer:buffer];
}

- (void)updateViewWithBuffer:(const uint32_t *)pixelBuffer {
    if (!pixelBuffer) return;

    size_t bufferSize = SCREEN_WIDTH * SCREEN_HEIGHT * 4;
    CFDataRef dataRef = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)pixelBuffer, bufferSize);
    if (!dataRef) return;

    CGDataProviderRef provider = CGDataProviderCreateWithCFData(dataRef);
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    
    CGImageRef imageRef = CGImageCreate(
        SCREEN_WIDTH, SCREEN_HEIGHT,
        8, 32, SCREEN_WIDTH * 4,
        colorSpace,
        kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst,
        provider, NULL, NO, kCGRenderingIntentDefault
    );
    
    if (imageRef) {
        // У отдельного CALayer смена contents по умолчанию анимируется (кроссфейд) - отключаем
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        self.emulationLayer.contents = (__bridge id _Nullable)(imageRef);
        [CATransaction commit];
        CGImageRelease(imageRef);
    }
    
    CGColorSpaceRelease(colorSpace);
    CGDataProviderRelease(provider);
    CFRelease(dataRef);
}

// Оригинальная матрица Радио-86РК / Орион-128
// Индексы соответствуют кнопкам виртуальной клавиатуры проекта
static const int rk_hardware_map[] = {
    // 0 - 14
    3 * 8 + 3, 2 * 8 + 1, 2 * 8 + 2, 2 * 8 + 3, 2 * 8 + 4, 2 * 8 + 5, 2 * 8 + 6, 2 * 8 + 7, 3 * 8 + 0, 3 * 8 + 1, 2 * 8 + 0, 3 * 8 + 5, 1 * 8 + 0, 1 * 8 + 1, 0,
    // 15 - 29
    5 * 8 + 2, 4 * 8 + 3, 6 * 8 + 5, 5 * 8 + 3, 4 * 8 + 5, 5 * 8 + 6, 4 * 8 + 7, 7 * 8 + 3, 7 * 8 + 5, 7 * 8 + 2, 5 * 8 + 0, 3 * 8 + 2, 1 * 8 + 2, 0, 0,
    // 30 - 44
    0, 4 * 8 + 6, 7 * 8 + 1, 6 * 8 + 7, 4 * 8 + 1, 6 * 8 + 0, 6 * 8 + 2, 5 * 8 + 7, 5 * 8 + 4, 4 * 8 + 4, 6 * 8 + 6, 7 * 8 + 4, 3 * 8 + 6, 1 * 8 + 3, 0,
    // 45 - 59
    0, 6 * 8 + 1, 7 * 8 + 6, 6 * 8 + 3, 5 * 8 + 5, 5 * 8 + 1, 6 * 8 + 4, 7 * 8 + 0, 4 * 8 + 2, 4 * 8 + 0, 3 * 8 + 4, 3 * 8 + 7, 0, 7 * 8 + 7, 7 * 8 + 7,
    // 60 - 71 (Добавленный функциональный блок, стрелки и СТР)
    0 * 8 + 0, 0 * 8 + 3, 0 * 8 + 1, 1 * 8 + 4, 1 * 8 + 5, 1 * 8 + 6, 0 * 8 + 4, 0 * 8 + 5, 0 * 8 + 6, 1 * 8 + 7, 0 * 8 + 2, 0
};

// Функция трансляции кодов клавиш macOS (Carbon) в индексы матрицы Ориона
static BOOL MapMacKeyToRK(unsigned short keyCode, int *out_row, int *out_col) {
    int target_idx = -1;
    
    switch (keyCode) {
        // --- Цифровой ряд ПК ---
        case kVK_ANSI_Grave:        target_idx = 0;  break; // Кнопка ` / Ё -> [;]
        case kVK_ANSI_1:            target_idx = 1;  break;
        case kVK_ANSI_2:            target_idx = 2;  break;
        case kVK_ANSI_3:            target_idx = 3;  break;
        case kVK_ANSI_4:            target_idx = 4;  break;
        case kVK_ANSI_5:            target_idx = 5;  break;
        case kVK_ANSI_6:            target_idx = 6;  break;
        case kVK_ANSI_7:            target_idx = 7;  break;
        case kVK_ANSI_8:            target_idx = 8;  break;
        case kVK_ANSI_9:            target_idx = 9;  break;
        case kVK_ANSI_0:            target_idx = 10; break;
        case kVK_ANSI_Minus:        target_idx = 11; break;
        
        // --- Буквенный ряд по QWERTY соответствию ---
        case kVK_ANSI_Q:            target_idx = 46; break; // Я / Q
        case kVK_ANSI_W:            target_idx = 33; break; // В / W
        case kVK_ANSI_E:            target_idx = 19; break; // Е / E
        case kVK_ANSI_R:            target_idx = 36; break; // Р / R
        case kVK_ANSI_T:            target_idx = 51; break; // Т / T
        case kVK_ANSI_Y:            target_idx = 32; break; // Ы / Y
        case kVK_ANSI_U:            target_idx = 17; break; // У / U
        case kVK_ANSI_I:            target_idx = 50; break; // И / I
        case kVK_ANSI_O:            target_idx = 37; break; // О / O
        case kVK_ANSI_P:            target_idx = 35; break; // П / P
        
        case kVK_ANSI_A:            target_idx = 34; break; // А / A
        case kVK_ANSI_S:            target_idx = 48; break; // С / S
        case kVK_ANSI_D:            target_idx = 39; break; // Д / D
        case kVK_ANSI_F:            target_idx = 31; break; // Ф / F
        case kVK_ANSI_G:            target_idx = 21; break; // Г / G
        case kVK_ANSI_H:            target_idx = 25; break; // Х / H
        case kVK_ANSI_J:            target_idx = 15; break; // Й / J
        case kVK_ANSI_K:            target_idx = 18; break; // К / K
        case kVK_ANSI_L:            target_idx = 38; break; // Л / L
        
        case kVK_ANSI_Z:            target_idx = 24; break; // З / Z
        case kVK_ANSI_X:            target_idx = 52; break; // Ь / X
        case kVK_ANSI_C:            target_idx = 16; break; // Ц / C
        case kVK_ANSI_V:            target_idx = 40; break; // Ж / V
        case kVK_ANSI_B:            target_idx = 53; break; // Б / B
        case kVK_ANSI_N:            target_idx = 20; break; // Н / N
        case kVK_ANSI_M:            target_idx = 49; break; // М / M
        
        // --- Пунктуация и спецсимволы ---
        case kVK_ANSI_LeftBracket:  target_idx = 22; break; // Ш / [
        case kVK_ANSI_RightBracket: target_idx = 23; break; // Щ / ]
        case kVK_ANSI_Semicolon:    target_idx = 26; break; // : / *
        case kVK_ANSI_Quote:        target_idx = 41; break; // Э / \
        case kVK_ANSI_Backslash:    target_idx = 41; break;
        case kVK_ANSI_Comma:        target_idx = 55; break; // Б / ,
        case kVK_ANSI_Period:       target_idx = 42; break; // Ю / .
        case kVK_ANSI_Slash:        target_idx = 56; break; // / / ?
        
        // --- Системные клавиши ---
        case kVK_Return:            target_idx = 27; break; // ВК (Enter)
        case kVK_Delete:            target_idx = 43; break; // ЗБ (Backspace)
        case kVK_Space:             target_idx = 58; break; // Пробел
        case kVK_Tab:               target_idx = 12; break; // ТАБ
        
        // --- Функциональный блок и стрелки ---
        case kVK_F1:                target_idx = 61; break;
        case kVK_F2:                target_idx = 66; break;
        case kVK_F3:                target_idx = 67; break;
        case kVK_F4:                target_idx = 68; break;
        case kVK_LeftArrow:         target_idx = 63; break;
        case kVK_UpArrow:           target_idx = 64; break;
        case kVK_RightArrow:        target_idx = 65; break;
        case kVK_DownArrow:         target_idx = 69; break;
        case kVK_Home:              target_idx = 60; break; // Команда "В угол"
        case kVK_PageUp:            target_idx = 62; break; // СТР
        
        default: break;
    }
    
    if (target_idx != -1) {
        int hw_code = rk_hardware_map[target_idx];
        *out_row = hw_code / 8;
        *out_col = hw_code % 8;
        return YES;
    }
    return NO;
}

// Маппинг клавиш macOS на системную матрицу клавиатуры Радио-86РК / Орион
- (void)handleKeyEvent:(NSEvent *)event isPressed:(BOOL)isPressed {
    unsigned short keyCode = [event keyCode];
    
    int row = -1, col = -1;
    if (MapMacKeyToRK(keyCode, &row, &col)) {
        if (row >= 0 && row < 8 && col >= 0 && col < 8) {
            orion128_press_key(row, col, isPressed);
        }
    }
}

- (void)flagsChanged:(NSEvent *)event {
    NSEventModifierFlags modifierFlags = [event modifierFlags];
    
    extern bool rk_cc_pressed;
    extern bool rk_us_pressed;
    extern bool rk_rus_lat_pressed;
    
    rk_cc_pressed = (modifierFlags & NSEventModifierFlagShift) != 0;
    rk_us_pressed = (modifierFlags & NSEventModifierFlagControl) != 0;
    
    // РУС/ЛАТ нажимается при каждой смене фиксатора Caps Lock и удерживается фиксированное время.
    // Раньше флаг висел до следующего события клавиатуры, а то и бесконечно.
    BOOL currentCapsLock = (modifierFlags & NSEventModifierFlagCapsLock) != 0;
    if (currentCapsLock != _lastCapsLock) {
        _lastCapsLock = currentCapsLock;
        rk_rus_lat_pressed = YES;
        NSUInteger token = ++_rusLatToken;
        __weak typeof(self) weakSelf = self;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(RUS_LAT_HOLD_SECONDS * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
            ViewController *strongSelf = weakSelf;
            if (strongSelf && strongSelf->_rusLatToken == token) {
                rk_rus_lat_pressed = NO;
            }
        });
    }
    
    [super flagsChanged:event];
}

- (BOOL)acceptsFirstResponder { return YES; }

- (void)keyDown:(NSEvent *)event {
    if (![event isARepeat]) {
        [self handleKeyEvent:event isPressed:YES];
    }
}

- (void)keyUp:(NSEvent *)event {
    [self handleKeyEvent:event isPressed:NO];
}

- (void)viewDidAppear {
    [super viewDidAppear];
    [self setupMenu];
    [self.view.window makeFirstResponder:self];
}

- (void)setupMenu {
    NSMenu *mainMenu = [NSApp mainMenu];
    NSMenuItem *fileMenuItem = [mainMenu itemWithTitle:@"File"];
    if (!fileMenuItem) {
        fileMenuItem = [[NSMenuItem alloc] initWithTitle:@"File" action:nil keyEquivalent:@""];
        [mainMenu addItem:fileMenuItem];
    }
    NSMenu *fileMenu = [fileMenuItem submenu] ? [fileMenuItem submenu] : [[NSMenu alloc] initWithTitle:@"File"];
    [fileMenuItem setSubmenu:fileMenu];
    
    [fileMenu addItem:[NSMenuItem separatorItem]];
    
    NSMenuItem *openItem = [[NSMenuItem alloc] initWithTitle:@"Open Orion File..."
                                                      action:@selector(triggerOpenFile:)
                                               keyEquivalent:@"o"];
    [openItem setTarget:self];
    [fileMenu addItem:openItem];
    
    NSMenuItem *resetItem = [[NSMenuItem alloc] initWithTitle:@"Hard Reset"
                                                       action:@selector(triggerReset:)
                                                keyEquivalent:@"r"];
    [resetItem setTarget:self];
    [fileMenu addItem:resetItem];
}

- (void)triggerOpenFile:(id)sender {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    
    // Ограничиваем выбор файлами .ord и .bin
    if (@available(macOS 11.0, *)) {
        panel.allowedContentTypes = @[
            [UTType typeWithFilenameExtension:@"ord"],
            [UTType typeWithFilenameExtension:@"rko"],
            [UTType typeWithFilenameExtension:@"bru"],
            [UTType typeWithFilenameExtension:@"bin"]
        ];
    } else {
        panel.allowedFileTypes = @[@"ord", @"rko", @"bru", @"bin"];
    }
    
    [panel beginSheetModalForWindow:self.view.window completionHandler:^(NSModalResponse result) {
        if (result == NSModalResponseOK) {
            NSURL *fileURL = panel.URL;
            if (fileURL) {
                // 1. Временно останавливаем таймер эмуляции, чтобы избежать конфликтов по памяти
                [self stopEmulation];
                
                // Buffer для сообщения об успехе/ошибке из Си-кода
                char msgBuffer[512] = {0};
                
                // 2. Вызываем Си-функцию загрузки
                BOOL success = orion128_load_file([fileURL.path UTF8String], msgBuffer, sizeof(msgBuffer));
                
                // 3. Показываем результат пользователю
                NSAlert *alert = [[NSAlert alloc] init];
                if (success) {
                    [alert setMessageText:@"Успешная загрузка"];
                    [alert setAlertStyle:NSAlertStyleInformational];
                } else {
                    [alert setMessageText:@"Ошибка загрузки"];
                    [alert setAlertStyle:NSAlertStyleWarning];
                }
                [alert setInformativeText:SafeUTF8String(msgBuffer)];
                [alert runModal];
                
                // 4. Перезапускаем эмуляцию (процессор начнет выполнять код с нового PC)
                [self startEmulation];
                NSLog(@"Orion-128 File Bootstrapped: %s", msgBuffer);
            }
        }
    }];
}

- (void)triggerReset:(id)sender {
    [self stopEmulation];
    orion128_reset();
    [self startEmulation];
    NSLog(@"Orion-128 Hardware Reset Triggered.");
}

@end
