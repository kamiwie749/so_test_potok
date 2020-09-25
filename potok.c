#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/wait>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/slab.h>//kmalloc()
#include <asm/uaccess.h>


#define OK 0
#define DEV_NAME "chardevpipe"
#define BUF_LEN 20

static int Major = 122;
static struct cdev *chcdev;

DEFINE_MUTEX(blokada);

//wskaźnik do bufora cyklicznego przechowującego dane modułu
static char *bufor;
static size_t rozmBufora;
static char *wskO, *wzkZ;//wskaźnik do zapisu i odczytu
static unsigned ileO, ileZ;//licznik czytających i zapisujących

//kolejki procesów usypianych przy odczycie i zapisie
static DECLARE_WAIT_QUEUE_HEAD(kolejkaO);
static DECLARE_WAIT_QUEUE_HEAD(kolejkaZ);

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .read = odczytUrzadzenia,
    .write = zapisUrzadzenia,
    .open = otwarcieUrzadzenia,
    .release = zamkniecieUrzadzenia,
};

module_init(chardevpipeInit);
module_exit(chardevpipeExit);

//deklaracja funkcji

MODULE_AUTHOR("Kamil Wieczorek");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Przykładowy moduł potoku danych");
//moduł używa urządzenia /dev/test
MODULE_SUPPORTED_DEVICE(DEV_NAME);


static int __init chardevpipeInit(void){
    int kodBledu;
    dev_t num;
    
    num = MKDEV(Major, 0);
    kodBledu = register_chardev_region(num,3,DEV_NAME);
    if(kodBledu < 0){
        printk(KERN_ALERT "Błąd rejestracji modułu %s z kodem %d\n",DEV_NAME,kodBledu);
        return kodBledu;
    }
    
    chcdev = cdev_alloc();
    chcdev->owner = THIS_MODULE;
    chcdev->ops = &fops;
    kodBledu = cdev_add(chcdev,num,3);
    if(kodBledu < 0){
        printk(KERN_ALERT "Nieudana próba zarejestrowania urządzenia w jądrze - zwrócony numer %d\n",kodBledu);
        unregister_chardev_region(num,3);
        return kodBledu;
    }
    printk(KERN_INFO "Rejestracja modułu OK\n");
    return OK;
}

staitic void __exit chardevpipeExit(void){
    dev_t num;
    num = MKNOD(Major,0);
    cdev_del(chcdev);
    unregister_chardev_region(num,3);
}

/*Funkcja pomocnicza pozwalająca obliczyć rozmiar wolnego miejsca w buforze cyklicznym*/
static inline int ileWolnego(void){
    /*Stosowane jest następujące podejście:
     * bufor nigdy nie jest wypełniany do końca, pomiędzy wskaźnikuem 
     * odczytu i zapisu jawsze pozostawiony jeden wolny bajt
     * wyjątkiem jest sytuacja nowo utworzonego bufora
     * co wykrywa poniższy if*/
    if(wskO == wskZ)
        return rozmBufora - 1;
    return ((wskO + rozmBufora - wskZ) % rozmBufora) - 1;
}


static int otwarcieUrzadzenia(struct inode *in, struct file *fp){
    //próba przejęcia semafora
    if(mutex_lock_interruptible(&blokada)){
        printk(KERN_INFO "Próba przejęcia semafora przerwana");
        return -ERESTARTSYS;
    }
    //mutex przejęty
    //przydział bufora dynamicznego, ze sprawdzeniem czy bufor już jest
    if(!bufor){
        //nie ma jescze bufora
        bufor = kmalloc(BUF_LEN, GPF_KERNEL); //ALOKACJA PAMIĘCI O ROZMIARZE BUF_LEN
        if(!bufor){
            PDEBUG("Nieudana próba przydziału pamięci dla bufora");
            mutex_unlock(&blokada);
            return -ENOMEM;
        }
        rozmBufora = BUF_LEN;
        wskO = wskZ = bufor;
    }
    
    //Rozróżniamy czy otwarcie do odczytu czy zapisu
    if(fp->f_mode & FMODE_READ){
        PDEBUG("Otwarcie do odczytu\n");
        ileO++;
    }
    if(fp->f_mode & FMODE_WRITE){
        PDEBUG("Otwarcie do zapisu\n");
        ileZ++;
    }
    mutex_unlock(&blokada);
    return OK;
}

static int zamkniecieUrzadzenia(struct inode *in, struct file *fp){
    mutex_lock_interruptible(&blokada);
    if(fp->f_mode & FMODE_READ){
        PDEBUG("Zamknięcie do odczytu\n");
        ileO--;
    }
    if(fp->f_mode & FMODE_WRITE){
        PDEBUG("Zamknięcie do zapisu\n");
        ileZ--;
    }
    //jeśli nikt już nie używa potoku, zwolnienie bufora
    if(ileO + ileZ == 0){
        kfree(bufor);
        bufor = NULL;//ważne bo tak znowu można utworzyć nowy bufor
    }
    mutex_unlock(&blokada);
    return OK;
}

static ssize_t odczytUrzadzenia(struct file *fp, char __user *buforUz, size_t rozmiar, loff_t *offset){
    size_t tmp;
    PDEBUG("Odczyt urzadzenia, rozmiar bufora %d\n", rozmiar);
    
    //przejęcie semafora
    if(mutex_lock_interruptible(&blokada)){
        printk(KERN_INFO "Próba przejęcia semafora nieudana\n");
        return -ERESTARTSYS;
    }
    
    //sprawdzenie czy w buforze są dane do odczytu
    while(wskO == wskZ){
        //brak danych, usypiamy proces
        mutex_unlock(&blokada);
        if(fp->f_flags & O_NONBLOCK){
            PDEBUG("Bufor pusty, lecz proces nie chce spać !!!\n");
            return -EAGAIN;
        }
        PDEBUG("Proces \%s\ uśpiony przy próbie odczytu\n",current->comm);
        if(wait_event_interruptible(kolejkaO,(wskO != wskZ))){
            return -ERESTARTSYS; //proces przerwany przez sygnał
        }
        //Po obudzeniu należy ponownie przejąć semafora //coś mi tu nie gra?????
        if(mutex_lock_interruptible(&blokada)){
            printk(KERN_INFO "Próba przejęcia semafora nieudana\n");
            return -ERESTARTSYS;
        }
    }
    
    //Na tym etapie wiadomo, że są dane do odczytu
    if(wskZ > wskO){
        tmp = wskZ - wskO;
        rozmiar = (rozmiar > tmp ? tmp : rozmiar);
    }
    else{
        //Ta sytuacja oznacza, że wskaźnik zapisu się "zawinął", wypiszemy dane do końca bufora (liniowo)
        tmp = bufor + rozmBufora - wskO;//?
        rozmiar = (rozmiar > tmp ? tmp : rozmiar);
    }
    if(copy_to_user(buforUz, wskO, rozmiar)){
        mutex_unlock(&blokada);
        return -EFAULT; //błąd zapisu
    }
    
    wskO += rozmiar;
    if(wskO == bufor + rozmBufora){//bufor to wskaźnik początku tu przsunięty na koniec 
        //Bufor odczytano do końca liniowo
        //przerzucenie wskaźnika odczytu na początek
        wskO = bufor;
    }
    mutex_unlock(&blokada);
    //obudzenie wszyskich zapisujących
    wake_up_interruptible(&kolejkaZ);
    PDEBUG("Proces \%s\ odczytał &li bajtów\n",current->comm, (long)rozmiar);
    return rozmiar;
}


static ssize_t zapisUrzadzenia(struct file *fp, const char __user *buforUz, size_t rozmiar, loff_t *offset){
    size_t tmp;
    PDEBUG("Zapis %d bajtów do urządzenia\n", rozmiar);
    if(mutex_lock_interruptible(&blokada)){
        printk(KERN_INFO "Próba przejęcia semafora przerwana\n");
        return -ERESTARTSYS;
    }
    //Sprawdzenie czy w buforze jest wolne miejsce
    if(ileWolnego() == 0){
        //w buforze nie ma miejsca
        mutex_unlock(&blokada);
        if(fp->f_flags & O_NONBLOCK){
            PDEBUG("Bufor pełny, a proces nie chce spać\n");
            return -EAGAIN;
        }
        PDEBUG("Proces \%s\ uśpiony przy próbie zapisu\n"current->comm);
        if(wait_event_interruptible(kolejkaZ, ileWolnego() > 0)){
            return -ERESTARTSYS; //proces przerwany przez sygnał
        }
        //Po obudzeniu należy ponownie przejąć semafora
        if(mutex_lock_interruptible(&blokada)){
        printk(KERN_INFO "Próba przejęcia semafora przerwana\n");
        return -ERESTARTSYS;
        }
    }
    
    // Na tym etapie wiadomo, że w buforze jest miejsce, zapisać należy mniejszą z wartości:
    //żądanej liczby bajtów i rozmiaru wolnego miejsca
    tmp = ileWolnego();
    rozmiar = (rozmiar > tmp ? tmp : rozmiar);
    if(wskZ >= wskO){
        //w tej sytuacji bufor zostaje zapisany do końca liniowo
        tmp = bufor + rozmBufora - wskZ;
        rozmiar = (rozmiar > tmp ? tmp : rozmiar);
    }
    else{
        //wskaźnik zapisu jest "przekręcony", zapisujemy do wskaźnika odczytu - 1
        tmp = wskO - wskZ - 1;
        rozmiar = (rozmiar > tmp ? tmp : rozmiar);
    }
    PDEBUG("Zapisane zostanie %li bajtów od adresu %p z bufora %p\n",(long)rozmiar,wskZ,buforUz);
    if(copy_from_user(wskZ,buforUz,rozmiar)){
        mutex_unlock(&blokada);
        return -EFAULT;
    }
    
    wskZ += rozmiar;
    if(wskZ == bufor + rozmBufora){
        //bufor zapisany do końca liniowo, wskaźnik zapisu na początek
        wskZ = bufor;
    }
    mutex_unlock(&blokada);
    //wybudzenie wszystkich procesów odczytujących
    wake_up_interruptible(&kolejkaO);
    PDEBUG("Proces \%s\ zapisał %li bajtów\n",current->comm, (long)rozmiar);
    
    return rozmiar;
}






